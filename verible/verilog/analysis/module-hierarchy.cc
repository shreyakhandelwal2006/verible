#include "verible/verilog/analysis/module-hierarchy.h"

#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "verible/common/text/concrete-syntax-leaf.h"
#include "verible/common/text/concrete-syntax-tree.h"
#include "verible/common/text/symbol.h"
#include "verible/common/text/tree-utils.h"
#include "verible/verilog/CST/declaration.h"
#include "verible/verilog/CST/module.h"
#include "verible/verilog/CST/parameters.h"
#include "verible/verilog/CST/verilog-nonterminals.h"

namespace verilog {
namespace analysis {

// Maps type-parameter names to resolved type names.
using ParamEnv = std::map<std::string, std::string>;

// Transitively resolve a type name through a parameter environment.
// Performs recursive lookup and detects cycles via a visited set
static std::string ResolveType(const std::string &name,
                               const ParamEnv &env) {
  std::string current = name;
  std::set<std::string> visited;
  while (true) {
    if (visited.count(current)) break;  // cycle — stop
    visited.insert(current);
    auto it = env.find(current);
    if (it == env.end()) break;  // not a param — concrete type
    current = it->second;
  }
  return current;
}

// Extract the base type name from a type-expression CST subtree.
// Handles:
//   pkg::cpu      -> "pkg::cpu"    (qualified name, no params)
//   Foo#(4)       -> "Foo"         (unqualified name, params stripped)
//   pkg::Foo#(int)-> "pkg::Foo"    (qualified name, params stripped)
//   cpu           -> "cpu"         (simple name)
static std::string ExtractBaseTypeName(const verible::Symbol &symbol) {
  const verible::Symbol *current = &symbol;
  // Descend through wrapper nodes to get to the identifier structure.
  for (int depth = 0; depth < 6; ++depth) {
    if (current->Kind() != verible::SymbolKind::kNode) break;
    const auto &node = verible::SymbolCastToNode(*current);
    auto tag = NodeEnum(node.Tag().tag);
    if (tag == NodeEnum::kQualifiedId || tag == NodeEnum::kUnqualifiedId) {
      break;  // found the identifier node
    }
    // Descend to first non-null child.
    const verible::Symbol *child = nullptr;
    for (const auto &c : node.children()) {
      if (c != nullptr && c->Kind() == verible::SymbolKind::kNode) {
        child = c.get();
        break;
      }
    }
    if (child == nullptr) break;
    current = child;
  }

  if (current->Kind() != verible::SymbolKind::kNode) {
    // Leaf — return its text directly.
    const auto *leaf = verible::GetLeftmostLeaf(*current);
    return leaf ? std::string(leaf->get().text()) : "";
  }

  const auto &id_node = verible::SymbolCastToNode(*current);
  auto id_tag = NodeEnum(id_node.Tag().tag);

  if (id_tag == NodeEnum::kUnqualifiedId) {
    // Simple or parameterized: kUnqualifiedId { SymbolIdentifier [, kActualParameterList] }
    const auto *leaf = verible::GetLeftmostLeaf(id_node);
    return leaf ? std::string(leaf->get().text()) : "";
  }

  if (id_tag == NodeEnum::kQualifiedId) {
    // Qualified: kQualifiedId { kUnqualifiedId, "::", kUnqualifiedId, ... }
    // Extract SymbolIdentifier from each kUnqualifiedId, join with "::".
    std::string result;
    for (const auto &child : id_node.children()) {
      if (child == nullptr) continue;
      if (child->Kind() == verible::SymbolKind::kNode) {
        auto child_tag = NodeEnum(child->Tag().tag);
        if (child_tag == NodeEnum::kUnqualifiedId) {
          const auto *leaf = verible::GetLeftmostLeaf(*child);
          if (leaf != nullptr) {
            if (!result.empty()) result += "::";
            result += std::string(leaf->get().text());
          }
        }
      }
    }
    return result;
  }

  // Fallback: leftmost leaf text.
  const auto *leaf = verible::GetLeftmostLeaf(*current);
  return leaf ? std::string(leaf->get().text()) : "";
}

// Extract type-parameter defaults from a module/interface declaration's
// formal parameter list.
// e.g.  module wrapper #(parameter type T = alu) -> {T: "alu"}
static ParamEnv ExtractTypeParamDefaults(const verible::Symbol &decl) {
  ParamEnv env;
  const verible::SyntaxTreeNode *param_list =
      GetParamDeclarationListFromModuleDeclaration(decl);
  if (param_list == nullptr) return env;

  const auto params = FindAllParamDeclarations(*param_list);
  for (const auto &p : params) {
    if (!IsParamTypeDeclaration(*p.match)) continue;

    const auto *type_assignment =
        GetTypeAssignmentFromParamDeclaration(*p.match);
    if (type_assignment == nullptr) continue;

    const auto *id_leaf =
        GetIdentifierLeafFromTypeAssignment(*type_assignment);
    if (id_leaf == nullptr) continue;
    std::string param_name(id_leaf->get().text());

    const auto *expr = GetExpressionFromTypeAssignment(*type_assignment);
    if (expr == nullptr) continue;
    std::string default_type = ExtractBaseTypeName(*expr);
    if (!default_type.empty()) {
      env[param_name] = default_type;
    }
  }
  return env;
}

// Extract instantiation-site parameter overrides from a kDataDeclaration.
// e.g.  wrapper #(.T(cpu)) u_w() -> {T: "cpu"}
static ParamEnv ExtractInstantiationParamOverrides(
    const verible::Symbol &data_declaration) {
  ParamEnv env;
  const verible::SyntaxTreeNode *param_list =
      GetParamListFromDataDeclaration(data_declaration);
  if (param_list == nullptr) return env;

  const auto named_params = FindAllNamedParams(*param_list);
  for (const auto &np : named_params) {
    const auto *name_leaf = GetNamedParamFromActualParam(*np.match);
    if (name_leaf == nullptr) continue;
    std::string param_name(name_leaf->get().text());

    const auto *paren_group = GetParenGroupFromActualParam(*np.match);
    if (paren_group == nullptr) continue;
    // The paren group has structure: '(' expression ')'.
    // We want the leftmost leaf of the expression inside.
    // Child 0 is '(', child 1 is the expression, child 2 is ')'.
    if (paren_group->size() < 2) continue;
    const verible::Symbol *inner = (*paren_group)[1].get();
    if (inner == nullptr) continue;
    std::string override_type = ExtractBaseTypeName(*inner);
    if (!override_type.empty()) {
      env[param_name] = override_type;
    }
  }
  return env;
}

// Helper: extract instantiated type name from a kDataDeclaration node.
// Returns the text of the leftmost leaf of the type-identifier subtree, or
// an empty string if the structure is not as expected.
static std::string ExtractInstantiatedTypeName(
    const verible::Symbol &data_declaration) {
  // GetTypeIdentifierFromDataDeclaration returns a Symbol* that may be a
  // kUnqualifiedId node, a kQualifiedId node, or a leaf. We just
  // want the leftmost leaf's text, which gives us the type name.
  const verible::Symbol *type_id =
      GetTypeIdentifierFromDataDeclaration(data_declaration);
  if (type_id == nullptr) return "";

  const verible::SyntaxTreeLeaf *leaf = verible::GetLeftmostLeaf(*type_id);
  if (leaf == nullptr) return "";

  // Gets the name of the type of the module being instantiated as a string
  // rather than a Symbol*.
  return std::string(leaf->get().text());
}

// Helper: extract instance name from a kGateInstance node.
// Returns the text of the instance name leaf (child 0), or an empty string
// if the structure is not as expected.
static std::string ExtractInstanceName(
    const verible::Symbol &gate_instance) {
  const verible::TokenInfo *name_token =
      GetModuleInstanceNameTokenInfoFromGateInstance(gate_instance);
  if (name_token == nullptr) return "";
  return std::string(name_token->text());
}

// Internal alias: use the public InstantiationRecord type.
using InstantiationInfo = InstantiationRecord;

// Collects per-module/interface instantiation info from a declaration node,
// using only the direct children of the kModuleItemList to avoid
// descending into nested module/interface definitions.
//
// Works for both kModuleDeclaration and kInterfaceDeclaration nodes, since
// GetModuleName() and GetModuleItemList() handle both.
//
// Returns the declaration name via "out_name" and appends instantiation
// records to "out_instances".  Returns false if the declaration could not be
// processed (e.g. missing name).
static bool CollectDirectInstantiations(
    const verible::Symbol &decl,
    std::string *out_name,
    std::vector<InstantiationInfo> *out_instances) {
  // Extract module/interface name.
  const verible::SyntaxTreeLeaf *name_leaf = GetModuleName(decl);
  if (name_leaf == nullptr) return false;

  *out_name = std::string(name_leaf->get().text());

  // Extract type-parameter defaults from this declaration so that
  // instantiations within the body can be resolved.
  const ParamEnv type_param_defaults = ExtractTypeParamDefaults(decl);

  // Get the item list (the body of the module/interface).
  const verible::SyntaxTreeNode *item_list = GetModuleItemList(decl);
  if (item_list == nullptr) return true;  // empty body, no children

  // Iterate only over *direct* children of the item list to avoid
  // descending into nested module/interface definitions.
  for (const auto &child_ptr : item_list->children()) {
    if (child_ptr == nullptr) continue;
    if (child_ptr->Kind() != verible::SymbolKind::kNode) continue;

    const auto &child_node =
        verible::SymbolCastToNode(*child_ptr);

    // We are only interested in kDataDeclaration nodes since it
    // contains instantiated module/interface.
    if (!child_node.MatchesTag(NodeEnum::kDataDeclaration)) continue;

    // Check if this data declaration contains gate instances
    // (i.e. it is a module/primitive instantiation).
    const auto gate_instances = FindAllGateInstances(*child_ptr);
    if (gate_instances.empty()) continue;

    // Extract the instantiated module/interface type name.
    std::string type_name =
        ExtractInstantiatedTypeName(*child_ptr);
    if (type_name.empty()) continue;

    // Extract instantiation-site parameter overrides (e.g. #(.T(cpu))).
    ParamEnv overrides =
        ExtractInstantiationParamOverrides(*child_ptr);

    // Record one entry per gate instance.
    for (const auto &gi : gate_instances) {
      InstantiationInfo info;
      info.type_name = type_name;
      info.instance_name = ExtractInstanceName(*gi.match);
      info.param_overrides = overrides;
      out_instances->push_back(info);
    }
  }
  return true;
}

// map for a single file: declaration_name -> list of instantiation records.
using ModuleInstantiationMap =
    std::map<std::string, std::vector<InstantiationRecord>>;

// Map from declaration name to its DeclarationKind.
using DeclarationKindMap = std::map<std::string, DeclarationKind>;

// Map from declaration name to its CST node (for param extraction).
using DeclarationNodeMap =
    std::map<std::string, const verible::Symbol *>;

// Recursively build an InstanceNode tree for a given module/interface type.
// "visited" tracks the set of types currently on the recursion stack to
// detect and break cycles.
// "param_env" is the parameter environment for resolving type parameters
// in the current module scope (overrides merged on top of defaults).
static InstanceNode BuildNodeRecursive(
    const std::string &instance_name,
    const std::string &module_type,
    const ModuleInstantiationMap &module_map,
    const DeclarationKindMap &kind_map,
    const DeclarationNodeMap &decl_map,
    const ParamEnv &param_env,
    std::set<std::string> *visited,
    std::vector<std::string> *errors) {
  // Resolve module_type through the parameter environment.
  // ResolveType recursive lookup and detects cycles.
  std::string resolved_type = ResolveType(module_type, param_env);

  InstanceNode node;
  node.instance_name = instance_name;
  node.module_type = resolved_type;

  // Set declaration kind.
  auto kind_it = kind_map.find(resolved_type);
  if (kind_it != kind_map.end()) {
    node.declaration_kind = kind_it->second;
  }

  // Guard against recursive instantiation.
  if (visited->count(resolved_type)) return node;

  auto it = module_map.find(resolved_type);
  if (it == module_map.end()) {
    // Undeclared module/interface — emit error.
    if (errors != nullptr && !instance_name.empty()) {
      std::ostringstream msg;
      msg << "error: undeclared module/interface '" << resolved_type << "'";
      msg << " (instance '" << instance_name << "')";
      errors->push_back(msg.str());
    }
    return node;
  }

  visited->insert(resolved_type);

  // Build the parameter environment for children of this module.
  // Start with the module's own type-parameter defaults, then
  // apply any overrides from the instantiation site.
  for (const auto &inst : it->second) {
    std::string child_type = ResolveType(inst.type_name, param_env);

    ParamEnv child_env;

    // Get formal defaults from the child module declaration.
    auto child_decl_it = decl_map.find(child_type);
    if (child_decl_it != decl_map.end() &&
        child_decl_it->second != nullptr) {
      child_env = ExtractTypeParamDefaults(*child_decl_it->second);
    }

    // Merge instantiation-site overrides on top of defaults.
    for (const auto &[key, value] : inst.param_overrides) {
      child_env[key] = value;
    }

    node.children.push_back(
        BuildNodeRecursive(inst.instance_name, child_type,
                           module_map, kind_map, decl_map,
                           child_env, visited, errors));
  }
  visited->erase(resolved_type);

  return node;
}

// Helper: process a list of declaration matches and populate the map.
static void ProcessDeclarations(
    const std::vector<verible::TreeSearchMatch> &decl_matches,
    DeclarationKind kind,
    ModuleInstantiationMap *module_map,
    DeclarationKindMap *kind_map,
    DeclarationNodeMap *decl_map,
    std::vector<std::string> *all_modules,
    std::vector<std::string> *errors) {
  for (const auto &match : decl_matches) {
    std::string name;
    std::vector<InstantiationRecord> instances;

    if (!CollectDirectInstantiations(*match.match, &name, &instances)) {
      continue;
    }

    // Check for duplicate declarations.
    if (module_map->count(name)) {
      if (errors != nullptr) {
        errors->push_back(
            "error: multiple declarations of module/interface '" + name + "'");
      }
      continue;  // skip duplicate
    }

    module_map->emplace(name, std::move(instances));
    (*kind_map)[name] = kind;
    (*decl_map)[name] = match.match;
    all_modules->push_back(name);
  }
}

FileDeclarations CollectDeclarations(const verible::Symbol &root) {
  FileDeclarations decls;

  // for module declarations
  ProcessDeclarations(FindAllModuleDeclarations(root),
                      DeclarationKind::kModule,
                      &decls.module_map, &decls.kind_map, &decls.decl_map,
                      &decls.all_modules, &decls.errors);

  // for interface declarations
  ProcessDeclarations(FindAllInterfaceDeclarations(root),
                      DeclarationKind::kInterface,
                      &decls.module_map, &decls.kind_map, &decls.decl_map,
                      &decls.all_modules, &decls.errors);

  return decls;
}

static BuildResult BuildForestFromMapsInternal(
    const ModuleInstantiationMap &module_map,
    const DeclarationKindMap &kind_map,
    const DeclarationNodeMap &decl_map,
    const std::vector<std::string> &all_modules) {
  BuildResult result;

  // identify top-level modules/interfaces (those not instantiated by any other
  // module/interface).
  std::set<std::string> instantiated_types;
  for (const auto &[name, instances] : module_map) {
    for (const auto &inst : instances) {
      instantiated_types.insert(inst.type_name);
    }
  }

  std::vector<std::string> top_level;
  for (const auto &mod : all_modules) {
    if (instantiated_types.count(mod) == 0) {
      top_level.push_back(mod);
    }
  }

  // If every declaration is instantiated by another (pure cycle), fall back
  // to treating all as roots.
  if (top_level.empty()) {
    top_level = all_modules;
  }

  // recursively build InstanceNode trees for each top level node.
  for (const auto &mod : top_level) {
    std::set<std::string> visited;
    // For top-level roots, initialize the param env from the module's own
    // formal type-parameter defaults (e.g. parameter type T = alu).
    ParamEnv root_env;
    auto root_decl_it = decl_map.find(mod);
    if (root_decl_it != decl_map.end() && root_decl_it->second != nullptr) {
      root_env = ExtractTypeParamDefaults(*root_decl_it->second);
    }
    result.forest.push_back(BuildNodeRecursive(
        "", mod, module_map, kind_map, decl_map, root_env,
        &visited, &result.errors));
  }

  return result;
}

// single file build function
BuildResult BuildInstanceForest(const verible::Symbol &root) {
  FileDeclarations decls = CollectDeclarations(root);
  BuildResult result = BuildForestFromMapsInternal(
      decls.module_map, decls.kind_map, decls.decl_map, decls.all_modules);
  // Prepend collection errors (duplicates) before build errors.
  result.errors.insert(result.errors.begin(),
                       decls.errors.begin(), decls.errors.end());
  return result;
}

// multi file build function
BuildResult BuildInstanceForestFromMaps(
    const std::map<std::string, std::vector<InstantiationRecord>> &module_map,
    const std::map<std::string, DeclarationKind> &kind_map,
    const std::map<std::string, const verible::Symbol *> &decl_map,
    const std::vector<std::string> &all_modules) {
  return BuildForestFromMapsInternal(module_map, kind_map, decl_map,
                                     all_modules);
}

// prints module/interface label
static std::string KindLabel(DeclarationKind kind) {
  switch (kind) {
    case DeclarationKind::kModule:
      return "module";
    case DeclarationKind::kInterface:
      return "interface";
    default:
      return "";
  }
}

// Recursive helper for tree printing.
// "prefix" is the string prepended to every line (accumulated indentation).
// "is_last" indicates whether this node is the last child of its parent.
static void PrintNodeRecursive(
    const InstanceNode &node,
    const std::string &prefix,
    bool is_last,
    bool is_root,
    std::ostringstream *out) {
  const std::string label = KindLabel(node.declaration_kind);

  if (is_root) {
    // Root nodes print with kind label if known.
    if (!label.empty()) {
      *out << label << " " << node.module_type << "\n";
    } else {
      *out << node.module_type << "\n";
    }
  } else {
    *out << prefix;
    *out << (is_last ? "└── " : "├── ");
    *out << node.instance_name << " : ";
    if (!label.empty()) {
      *out << label << " ";
    }
    *out << node.module_type << "\n";
  }

  const std::string child_prefix =
      is_root ? "" : prefix + (is_last ? "    " : "│   ");

  for (std::size_t i = 0; i < node.children.size(); ++i) {
    const bool child_is_last = (i + 1 == node.children.size());
    PrintNodeRecursive(node.children[i], child_prefix, child_is_last,
                       false, out);
  }
}

std::string PrintHierarchyTree(const std::vector<InstanceNode> &forest) {
  std::ostringstream out;
  for (const auto &root : forest) {
    PrintNodeRecursive(root, "", true, true, &out);
  }
  return out.str();
}

}  // namespace analysis
}  // namespace verilog

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
#include "verible/verilog/CST/verilog-nonterminals.h"

namespace verilog {
namespace analysis {

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

// Holds one instantiation record: the instance label and the type being
// instantiated.
struct InstantiationInfo {
  std::string instance_name;  // e.g. "u_alu_1"
  std::string type_name;      // e.g. "alu"
};

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
    const std::string type_name =
        ExtractInstantiatedTypeName(*child_ptr);
    if (type_name.empty()) continue;

    // Record one entry per gate instance.
    for (const auto &gi : gate_instances) {
      InstantiationInfo info;
      info.type_name = type_name;
      info.instance_name = ExtractInstanceName(*gi.match);
      out_instances->push_back(info);
    }
  }
  return true;
}

// map for a single file: declaration_name -> list of instantiation records.
using ModuleInstantiationMap =
    std::map<std::string, std::vector<InstantiationInfo>>;

// Recursively build an InstanceNode tree for a given module/interface type.
// "visited" tracks the set of types currently on the recursion stack to
// detect and break cycles.
static InstanceNode BuildNodeRecursive(
    const std::string &instance_name,
    const std::string &module_type,
    const ModuleInstantiationMap &module_map,
    std::set<std::string> *visited) {
  InstanceNode node;
  node.instance_name = instance_name;
  node.module_type = module_type;

  // Guard against recursive instantiation.
  if (visited->count(module_type)) return node;

  auto it = module_map.find(module_type);
  if (it == module_map.end()) return node;  // unknown type, leaf

  visited->insert(module_type);
  for (const auto &inst : it->second) {
    node.children.push_back(
        BuildNodeRecursive(inst.instance_name, inst.type_name,
                           module_map, visited));
  }
  visited->erase(module_type);

  return node;
}

// Helper: process a list of declaration matches and populate the map.
static void ProcessDeclarations(
    const std::vector<verible::TreeSearchMatch> &decl_matches,
    ModuleInstantiationMap *module_map,
    std::vector<std::string> *all_modules) {
  for (const auto &match : decl_matches) {
    std::string name;
    std::vector<InstantiationInfo> instances;

    if (!CollectDirectInstantiations(*match.match, &name, &instances)) {
      continue;
    }

    module_map->emplace(name, std::move(instances));
    all_modules->push_back(name);
  }
}

std::vector<InstanceNode> BuildInstanceForest(const verible::Symbol &root) {
  // collect per-module and per-interface instantiation info for the file.
  ModuleInstantiationMap module_map;
  std::vector<std::string> all_modules;  // preserve declaration order

  // for module declarations
  ProcessDeclarations(FindAllModuleDeclarations(root),
                      &module_map, &all_modules);

  // for interface declarations
  ProcessDeclarations(FindAllInterfaceDeclarations(root),
                      &module_map, &all_modules);

  // identify top-level modules/interfaces (those not instantiated by any other
  // module/interface in the file).
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
  std::vector<InstanceNode> forest;
  for (const auto &mod : top_level) {
    std::set<std::string> visited;
    forest.push_back(BuildNodeRecursive("", mod, module_map, &visited));
  }

  return forest;
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
  if (is_root) {
    // Root nodes just print the module type.
    *out << node.module_type << "\n";
  } else {
    *out << prefix;
    *out << (is_last ? "└── " : "├── ");
    *out << node.instance_name << " : " << node.module_type << "\n";
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

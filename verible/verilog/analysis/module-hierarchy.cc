#include "verible/verilog/analysis/module-hierarchy.h"

#include <map>
#include <string>
#include <vector>

#include "verible/common/text/concrete-syntax-leaf.h"
#include "verible/common/text/symbol.h"
#include "verible/common/text/tree-utils.h"
#include "verible/verilog/CST/declaration.h"
#include "verible/verilog/CST/module.h"

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

// Builds the parent->child hierarchy from a parsed CST root.

ModuleHierarchyMap BuildModuleHierarchy(const verible::Symbol &root) {
  ModuleHierarchyMap result;

  // Finds every module declaration in the file.
  const auto module_decls = FindAllModuleDeclarations(root);

  for (const auto &mod_match : module_decls) {
    // Extracts the declaring module's name.
    const verible::SyntaxTreeLeaf *name_leaf = GetModuleName(*mod_match.match);
    if (name_leaf == nullptr) continue;

    const std::string parent_name(name_leaf->get().text());

    // Ensure the parent always appears as a key, even with no children.
    auto &children = result[parent_name];

    // Gets the module item list (the body of the module).
    const verible::SyntaxTreeNode *item_list =
        GetModuleItemList(*mod_match.match);
    if (item_list == nullptr) continue;

    // Finds all kDataDeclaration nodes inside the module body that
    // contain at least one kGateInstance child.
    // Order: kDataDeclaration -> kInstantiationBase ->
    //        kGateInstanceRegisterVariableList -> kGateInstance

    const auto data_decls = FindAllDataDeclarations(*item_list);

    for (const auto &decl_match : data_decls) {
      // FindAllGateInstances searches the given subtree for kGateInstance
      // nodes. If this data declaration contains at least one kGateInstance it
      // is a module/primitive instantiation.
      const auto gate_instances = FindAllGateInstances(*decl_match.match);
      if (gate_instances.empty()) continue;

      // Extract the instantiated module type name.
      const std::string type_name =
          ExtractInstantiatedTypeName(*decl_match.match);
      if (type_name.empty()) continue;

      // For declarations with multiple instances of the same type
      // we record one entry per gate instance so that the list length reflects
      // actual instantiation count.
      for (std::size_t i = 0; i < gate_instances.size(); ++i) {
        children.push_back(type_name);
      }
    }
  }

  return result;
}

}  // namespace analysis
}  // namespace verilog

// Module hierarchy extraction for SystemVerilog.
//
// Two modes of operation:
//   1. Single-file: BuildInstanceForest() parses one CST and returns a forest.
//   2. Multi-file:  CollectDeclarations() extracts raw maps from each file,
//                   the caller merges them, then BuildInstanceForestFromMaps()
//                   builds the final forest from the merged maps.
//
// Features:
//  * direct (non-generate-block) module/interface instantiations
//  * parameter type elaboration (defaults + instantiation-site overrides)
//  * duplicate declaration detection
//  * undeclared module/interface detection
//  * module/interface classification

#ifndef VERIBLE_VERILOG_ANALYSIS_MODULE_HIERARCHY_H_
#define VERIBLE_VERILOG_ANALYSIS_MODULE_HIERARCHY_H_

#include <map>
#include <string>
#include <vector>

#include "verible/common/text/symbol.h"

namespace verilog {
namespace analysis {

// Distinguishes whether a hierarchy node originated from a module or
// interface declaration.
enum class DeclarationKind {
  kModule,
  kInterface,
  kUnknown,  // for types not found in any declaration
};

// A single node in the instantiation hierarchy tree.
// Each node represents one module/interface instantiation, or a top-level
// module/interface (in which case instance_name is empty).
struct InstanceNode {
  std::string instance_name;  // e.g. "u_cpu", empty for a top-level root
  std::string module_type;    // e.g. "cpu"
  DeclarationKind declaration_kind = DeclarationKind::kUnknown;
  std::vector<InstanceNode> children;
};

// Result of building a hierarchy forest.  Contains the tree(s) and any
// diagnostic error messages produced during construction.
struct BuildResult {
  std::vector<InstanceNode> forest;
  std::vector<std::string> errors;
};

// One instantiation record: instance label, type, and param overrides.
struct InstantiationRecord {
  std::string instance_name;  // e.g. "u_alu_1"
  std::string type_name;      // e.g. "alu"
  std::map<std::string, std::string> param_overrides;  // e.g. {T: "cpu"}
};

// Raw declaration data extracted from a single file.
// Intended to be merged across files before building the forest.
struct FileDeclarations {
  // module/interface name -> list of instantiation records.
  std::map<std::string, std::vector<InstantiationRecord>> module_map;
  // module/interface name -> kind (module or interface).
  std::map<std::string, DeclarationKind> kind_map;
  // module/interface name -> CST node pointer (for param extraction).
  // IMPORTANT: these pointers are only valid as long as the VerilogAnalyzer
  // that produced the CST is alive.
  std::map<std::string, const verible::Symbol *> decl_map;
  // Declaration names in order of appearance.
  std::vector<std::string> all_modules;
  // Any errors found during collection (e.g. duplicate declarations).
  std::vector<std::string> errors;
};

// Build a forest of instance hierarchy trees from a parsed CST root.
//
// Each tree root corresponds to a top-level module or interface (i.e. one
// that is not instantiated by any other module/interface in the same file).
// Children are recursively expanded using the per-module instantiation
// information.
//
// Both module declarations and interface declarations are discovered.
//
// Instance names, module/interface types, declaration kinds, and
// parent-child hierarchy relationships are all preserved.
//
// Parameter type elaboration is performed: if a module declares
// `parameter type T = alu` and an instantiation site overrides it with
// `#(.T(cpu))`, the hierarchy will show the concrete type `cpu`.
//
// Errors are emitted for:
//  - undeclared module/interface references
//  - multiple declarations of the same module/interface name
BuildResult BuildInstanceForest(const verible::Symbol &root);

// Collect raw declaration data from a single file's CST.
// Does NOT build the tree — only populates the maps.
// The returned FileDeclarations holds pointers into the CST, so the
// VerilogAnalyzer that produced `root` must outlive the returned struct.
FileDeclarations CollectDeclarations(const verible::Symbol &root);

// Build a hierarchy forest from globally-merged maps.
// This is the multi-file entry point: call CollectDeclarations() per file,
// merge the maps, then call this to produce the final forest.
BuildResult BuildInstanceForestFromMaps(
    const std::map<std::string, std::vector<InstantiationRecord>> &module_map,
    const std::map<std::string, DeclarationKind> &kind_map,
    const std::map<std::string, const verible::Symbol *> &decl_map,
    const std::vector<std::string> &all_modules);

// Pretty-print a hierarchy forest as an indented tree.
// Includes declaration kind labels (module/interface) in the output.
std::string PrintHierarchyTree(const std::vector<InstanceNode> &forest);

}  // namespace analysis
}  // namespace verilog

#endif  // VERIBLE_VERILOG_ANALYSIS_MODULE_HIERARCHY_H_

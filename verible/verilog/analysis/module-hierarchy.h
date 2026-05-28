// This provides a single-file module hierarchy extraction pass for
// SystemVerilog.
//
// Scope of this pass:
//  * single-file extraction only
//  * direct (non-generate-block) module/interface instantiations only
//  * no cross-file resolution, no parameter specialisation

#ifndef VERIBLE_VERILOG_ANALYSIS_MODULE_HIERARCHY_H_
#define VERIBLE_VERILOG_ANALYSIS_MODULE_HIERARCHY_H_

#include <string>
#include <vector>

#include "verible/common/text/symbol.h"

namespace verilog {
namespace analysis {

// A single node in the instantiation hierarchy tree.
// Each node represents one module/interface instantiation, or a top-level
// module/interface (in which case instance_name is empty).
struct InstanceNode {
  std::string instance_name;  // e.g. "u_cpu", empty for a top-level root
  std::string module_type;    // e.g. "cpu"
  std::vector<InstanceNode> children;
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
// Instance names, module/interface types, and parent-child hierarchy
// relationships are all preserved.
std::vector<InstanceNode> BuildInstanceForest(const verible::Symbol &root);

// Pretty-print a hierarchy forest as an indented tree.
std::string PrintHierarchyTree(const std::vector<InstanceNode> &forest);

}  // namespace analysis
}  // namespace verilog

#endif  // VERIBLE_VERILOG_ANALYSIS_MODULE_HIERARCHY_H_

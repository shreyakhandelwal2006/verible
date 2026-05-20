// This provides a single-file module hierarchy extraction pass for
// SystemVerilog.
//
// Scope of this pass:
//  * single-file extraction only
//  * direct (non-generate-block) module instantiations only
//  * no cross-file resolution, no parameter specialisation

#ifndef VERIBLE_VERILOG_ANALYSIS_MODULE_HIERARCHY_H_
#define VERIBLE_VERILOG_ANALYSIS_MODULE_HIERARCHY_H_

#include <map>
#include <string>
#include <vector>

#include "verible/common/text/symbol.h"

namespace verilog {
namespace analysis {

// Maps a module name to the (possibly-duplicated) list of module type names
// that are instantiated directly inside it.
//
// The map key is the declaring module name.
// Each entry in the value vector is the type name of one instantiation
// (i.e. the module being instantiated, not the instance label).
//
// Modules with no instantiations still appear as keys with an empty vector,
// provided they have a parseable name.
using ModuleHierarchyMap = std::map<std::string, std::vector<std::string>>;

// Build the parent->child hierarchy from a parsed CST root.
// Returns a ModuleHierarchyMap as described above.
ModuleHierarchyMap BuildModuleHierarchy(const verible::Symbol &root);

}  // namespace analysis
}  // namespace verilog

#endif  // VERIBLE_VERILOG_ANALYSIS_MODULE_HIERARCHY_H_

// verible-verilog-hierarchy is a command-line tool for extracting and
// displaying module/interface instantiation hierarchies from
// Verilog/SystemVerilog source files.
//
// Usage:
//   verible-verilog-hierarchy <file> [<file>...]
//
// Produces a tree view of the instance hierarchy.

#include <functional>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "verible/common/strings/mem-block.h"
#include "verible/common/util/file-util.h"
#include "verible/common/util/init-command-line.h"
#include "verible/common/util/iterator-range.h"
#include "verible/verilog/analysis/module-hierarchy.h"
#include "verible/verilog/analysis/verilog-analyzer.h"
#include "verible/verilog/preprocessor/verilog-preprocess.h"

using verilog::VerilogAnalyzer;
using verilog::analysis::BuildInstanceForest;
using verilog::analysis::InstanceNode;
using verilog::analysis::PrintHierarchyTree;

// Build the combined forest from multiple files.
// Each file is parsed independently; per-file module/interface instantiation
// data is merged into a combined map, then a combined forest is built.
static std::vector<InstanceNode> BuildCombinedForest(
    const std::vector<std::string> &filenames,
    const verilog::VerilogPreprocess::Config &preprocess_config,
    int *exit_status) {
  // Combined map: declaration_name -> list of {instance_name, type_name}
  struct InstInfo {
    std::string instance_name;
    std::string type_name;
  };
  std::map<std::string, std::vector<InstInfo>> combined_map;
  std::vector<std::string> all_modules;  // preserve order

  for (const auto &filename : filenames) {
    auto content_status = verible::file::GetContentAsMemBlock(filename);
    if (!content_status.status().ok()) {
      std::cerr << content_status.status().message() << std::endl;
      *exit_status = 1;
      continue;
    }
    std::shared_ptr<verible::MemBlock> content = std::move(*content_status);

    auto analyzer = VerilogAnalyzer::AnalyzeAutomaticMode(
        content, filename, preprocess_config);
    if (analyzer == nullptr) {
      std::cerr << filename << ": analysis failed" << std::endl;
      *exit_status = 1;
      continue;
    }
    if (!analyzer->LexStatus().ok()) {
      std::cerr << filename << ": lex error: "
                << analyzer->LexStatus().message() << std::endl;
      *exit_status = 1;
      continue;
    }
    if (!analyzer->ParseStatus().ok()) {
      std::cerr << filename << ": parse error: "
                << analyzer->ParseStatus().message() << std::endl;
      *exit_status = 1;
      continue;
    }

    const auto &tree = analyzer->SyntaxTree();
    if (tree == nullptr) continue;

    // Build per-file forest (now includes both modules and interfaces).
    auto forest = BuildInstanceForest(*tree);

    // Walk per-file forest to merge instantiation data into combined map.
    std::function<void(const InstanceNode &)> collect =
        [&](const InstanceNode &node) {
          auto &insts = combined_map[node.module_type];
          for (const auto &child : node.children) {
            insts.push_back({child.instance_name, child.module_type});
          }
          for (const auto &child : node.children) {
            collect(child);
          }
        };

    for (const auto &root : forest) {
      if (combined_map.find(root.module_type) == combined_map.end()) {
        all_modules.push_back(root.module_type);
      }
      collect(root);
    }
  }

  // Identify top-level modules/interfaces.
  std::set<std::string> instantiated_types;
  for (const auto &[name, instances] : combined_map) {
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
  if (top_level.empty()) {
    top_level = all_modules;
  }

  // Build combined trees with cycle detection.
  std::function<InstanceNode(const std::string &, const std::string &,
                              std::set<std::string> *)>
      build_node = [&](const std::string &inst_name,
                       const std::string &mod_type,
                       std::set<std::string> *visited) -> InstanceNode {
    InstanceNode node;
    node.instance_name = inst_name;
    node.module_type = mod_type;
    if (visited->count(mod_type)) return node;
    auto it = combined_map.find(mod_type);
    if (it == combined_map.end()) return node;
    visited->insert(mod_type);
    for (const auto &inst : it->second) {
      node.children.push_back(
          build_node(inst.instance_name, inst.type_name, visited));
    }
    visited->erase(mod_type);
    return node;
  };

  std::vector<InstanceNode> forest;
  for (const auto &mod : top_level) {
    std::set<std::string> visited;
    forest.push_back(build_node("", mod, &visited));
  }
  return forest;
}

int main(int argc, char **argv) {
  const auto usage = absl::StrCat(
      "usage: ", argv[0], " [options] <file> [<file>...]\n\n"
      "Extracts and displays module/interface instantiation hierarchies\n"
      "from Verilog/SystemVerilog source files.");
  const auto args = verible::InitCommandLine(usage, &argc, &argv);

  if (args.size() <= 1) {
    std::cerr << "No input files specified." << std::endl;
    return 1;
  }

  const verilog::VerilogPreprocess::Config preprocess_config{
      .filter_branches = true,
  };

  int exit_status = 0;

  // Collect filenames.
  std::vector<std::string> filenames;
  for (auto it = args.begin() + 1; it != args.end(); ++it) {
    filenames.emplace_back(*it);
  }

  // Build combined forest from all input files and print.
  auto forest =
      BuildCombinedForest(filenames, preprocess_config, &exit_status);
  std::cout << PrintHierarchyTree(forest);

  return exit_status;
}

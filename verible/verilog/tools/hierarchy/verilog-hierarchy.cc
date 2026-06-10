// verible-verilog-hierarchy is a command-line tool for extracting and
// displaying module/interface instantiation hierarchies from
// Verilog/SystemVerilog source files.
//
// Usage:
//   verible-verilog-hierarchy <file> [<file>...]
//
// Produces a tree view of the instance hierarchy.
//
// Multi-file strategy (global-map approach):
//   1. Parse each file and call CollectDeclarations() to extract raw maps.
//   2. Merge all per-file maps into a single global set of maps.
//   3. Call BuildInstanceForestFromMaps() once on the merged maps.
//
// This ensures that cross-file references, parameter overrides, and
// declaration kinds are all resolved correctly.

#include <iostream>
#include <map>
#include <memory>
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
using verilog::analysis::BuildInstanceForestFromMaps;
using verilog::analysis::CollectDeclarations;
using verilog::analysis::DeclarationKind;
using verilog::analysis::FileDeclarations;
using verilog::analysis::InstantiationRecord;
using verilog::analysis::PrintHierarchyTree;

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

  // Phase 1: Parse each file and collect raw declaration data.
  // Keep analyzers alive so that CST node pointers in decl_map remain valid.
  std::vector<std::unique_ptr<VerilogAnalyzer>> analyzers;
  std::vector<FileDeclarations> per_file_decls;

  for (const auto &filename : filenames) {
    auto content_status = verible::file::GetContentAsMemBlock(filename);
    if (!content_status.status().ok()) {
      std::cerr << content_status.status().message() << std::endl;
      exit_status = 1;
      continue;
    }
    std::shared_ptr<verible::MemBlock> content = std::move(*content_status);

    auto analyzer = VerilogAnalyzer::AnalyzeAutomaticMode(
        content, filename, preprocess_config);
    if (analyzer == nullptr) {
      std::cerr << filename << ": analysis failed" << std::endl;
      exit_status = 1;
      continue;
    }
    if (!analyzer->LexStatus().ok()) {
      std::cerr << filename << ": lex error: "
                << analyzer->LexStatus().message() << std::endl;
      exit_status = 1;
      continue;
    }
    if (!analyzer->ParseStatus().ok()) {
      std::cerr << filename << ": parse error: "
                << analyzer->ParseStatus().message() << std::endl;
      exit_status = 1;
      continue;
    }

    const auto &tree = analyzer->SyntaxTree();
    if (tree == nullptr) {
      analyzers.push_back(std::move(analyzer));
      continue;
    }

    // Collect raw declaration data from this file.
    FileDeclarations decls = CollectDeclarations(*tree);

    // Emit per-file collection errors (e.g. intra-file duplicates).
    for (const auto &err : decls.errors) {
      std::cerr << filename << ": " << err << std::endl;
      exit_status = 1;
    }

    per_file_decls.push_back(std::move(decls));

    // Keep analyzer alive — decl_map pointers reference its CST.
    analyzers.push_back(std::move(analyzer));
  }

  // Phase 2: Merge per-file maps into global maps.
  std::map<std::string, std::vector<InstantiationRecord>> global_module_map;
  std::map<std::string, DeclarationKind> global_kind_map;
  std::map<std::string, const verible::Symbol *> global_decl_map;
  std::vector<std::string> global_all_modules;

  for (const auto &decls : per_file_decls) {
    for (const auto &name : decls.all_modules) {
      // Check for cross-file duplicate declarations.
      if (global_module_map.count(name)) {
        std::cerr << "error: multiple declarations of module/interface '"
                  << name << "'" << std::endl;
        exit_status = 1;
        continue;
      }

      auto it = decls.module_map.find(name);
      if (it != decls.module_map.end()) {
        global_module_map[name] = it->second;
      }

      auto kind_it = decls.kind_map.find(name);
      if (kind_it != decls.kind_map.end()) {
        global_kind_map[name] = kind_it->second;
      }

      auto decl_it = decls.decl_map.find(name);
      if (decl_it != decls.decl_map.end()) {
        global_decl_map[name] = decl_it->second;
      }

      global_all_modules.push_back(name);
    }
  }

  // Phase 3: Build the combined forest from the global maps.
  auto result = BuildInstanceForestFromMaps(
      global_module_map, global_kind_map, global_decl_map, global_all_modules);

  // Emit build errors (undeclared modules, etc.).
  for (const auto &err : result.errors) {
    std::cerr << err << std::endl;
    exit_status = 1;
  }

  // Phase 4: Print the hierarchy.
  std::cout << PrintHierarchyTree(result.forest);

  return exit_status;
}

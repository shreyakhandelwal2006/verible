#include "verible/verilog/analysis/module-hierarchy.h"

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "verible/verilog/analysis/verilog-analyzer.h"
#include "verible/verilog/preprocessor/verilog-preprocess.h"

namespace verilog {
namespace analysis {
namespace {

static constexpr VerilogPreprocess::Config kDefaultPreprocess;

static BuildResult ResultFromSource(const std::string &source) {
  auto analyzer = verilog::VerilogAnalyzer::AnalyzeAutomaticMode(
      source, "<test>", kDefaultPreprocess);
  if (analyzer == nullptr) return {};
  if (!analyzer->LexStatus().ok()) return {};
  if (!analyzer->ParseStatus().ok()) return {};
  const auto &tree = analyzer->SyntaxTree();
  if (tree == nullptr) return {};
  return BuildInstanceForest(*tree);
}

// Convenience: just the forest.
static std::vector<InstanceNode> ForestFromSource(const std::string &source) {
  return ResultFromSource(source).forest;
}

// BuildInstanceForest tests

TEST(InstanceForestTest, EmptySource) {
  auto forest = ForestFromSource("");
  EXPECT_TRUE(forest.empty());
}

TEST(InstanceForestTest, SingleModuleNoChildren) {
  const std::string src = "module top; endmodule\n";
  auto forest = ForestFromSource(src);
  ASSERT_EQ(forest.size(), 1u);
  EXPECT_EQ(forest[0].module_type, "top");
  EXPECT_TRUE(forest[0].instance_name.empty());
  EXPECT_TRUE(forest[0].children.empty());
}

TEST(InstanceForestTest, SimpleParentChild) {
  // a instantiates b once.
  const std::string src =
      "module b; endmodule\n"
      "module a;\n"
      "  b u_b();\n"
      "endmodule\n";
  auto forest = ForestFromSource(src);

  // Only a is top-level (b is instantiated).
  ASSERT_EQ(forest.size(), 1u);
  EXPECT_EQ(forest[0].module_type, "a");

  ASSERT_EQ(forest[0].children.size(), 1u);
  EXPECT_EQ(forest[0].children[0].instance_name, "u_b");
  EXPECT_EQ(forest[0].children[0].module_type, "b");
  EXPECT_TRUE(forest[0].children[0].children.empty());
}

TEST(InstanceForestTest, MultipleInstancesSameType) {
  // a instantiates b twice in one line.
  const std::string src =
      "module b; endmodule\n"
      "module a;\n"
      "  b u1(), u2();\n"
      "endmodule\n";
  auto forest = ForestFromSource(src);

  ASSERT_EQ(forest.size(), 1u);
  EXPECT_EQ(forest[0].module_type, "a");

  // Two gate instances -> two children.
  ASSERT_EQ(forest[0].children.size(), 2u);
  EXPECT_EQ(forest[0].children[0].instance_name, "u1");
  EXPECT_EQ(forest[0].children[0].module_type, "b");
  EXPECT_EQ(forest[0].children[1].instance_name, "u2");
  EXPECT_EQ(forest[0].children[1].module_type, "b");
}

TEST(InstanceForestTest, TwoDifferentInstantiations) {
  const std::string src =
      "module b; endmodule\n"
      "module reg_file; endmodule\n"
      "module a;\n"
      "  b     u_b();\n"
      "  c     u_c();\n"
      "endmodule\n";
  auto result = ResultFromSource(src);
  const auto &forest = result.forest;

  // a is top-level (b and reg_file are defined but reg_file is not
  // instantiated — however c is not defined, so it's unknown).
  // Top-level: a and reg_file (reg_file is never instantiated).
  ASSERT_EQ(forest.size(), 2u);

  // Find 'a' in the forest.
  const InstanceNode *a_node = nullptr;
  for (const auto &root : forest) {
    if (root.module_type == "a") {
      a_node = &root;
      break;
    }
  }
  ASSERT_NE(a_node, nullptr);

  ASSERT_EQ(a_node->children.size(), 2u);
  // Order is declaration order.
  EXPECT_EQ(a_node->children[0].instance_name, "u_b");
  EXPECT_EQ(a_node->children[0].module_type, "b");
  EXPECT_EQ(a_node->children[1].instance_name, "u_c");
  EXPECT_EQ(a_node->children[1].module_type, "c");

  // 'c' is undeclared, so there should be an error.
  EXPECT_FALSE(result.errors.empty());
  bool found_undeclared_c = false;
  for (const auto &err : result.errors) {
    if (err.find("undeclared") != std::string::npos &&
        err.find("'c'") != std::string::npos) {
      found_undeclared_c = true;
    }
  }
  EXPECT_TRUE(found_undeclared_c);
}

TEST(InstanceForestTest, MultiLevelHierarchy) {
  // a -> b -> c (3 levels)
  const std::string src =
      "module c; endmodule\n"
      "module b;\n"
      "  c u_c();\n"
      "endmodule\n"
      "module a;\n"
      "  b u_b();\n"
      "endmodule\n";
  auto forest = ForestFromSource(src);

  // Only a is top-level.
  ASSERT_EQ(forest.size(), 1u);
  EXPECT_EQ(forest[0].module_type, "a");

  ASSERT_EQ(forest[0].children.size(), 1u);
  const auto &b = forest[0].children[0];
  EXPECT_EQ(b.instance_name, "u_b");
  EXPECT_EQ(b.module_type, "b");

  ASSERT_EQ(b.children.size(), 1u);
  EXPECT_EQ(b.children[0].instance_name, "u_c");
  EXPECT_EQ(b.children[0].module_type, "c");
  EXPECT_TRUE(b.children[0].children.empty());
}

TEST(InstanceForestTest, AluCpuSocHierarchy) {

  const std::string src =
      "module alu; endmodule\n"
      "module cpu;\n"
      "  alu u_alu_1();\n"
      "  alu u_alu_2();\n"
      "endmodule\n"
      "module soc;\n"
      "  cpu u_cpu();\n"
      "endmodule\n";
  auto forest = ForestFromSource(src);

  // Only soc is a top-level module (alu and cpu are instantiated).
  ASSERT_EQ(forest.size(), 1u);
  const auto &soc = forest[0];
  EXPECT_EQ(soc.module_type, "soc");
  EXPECT_TRUE(soc.instance_name.empty());

  // soc has one child: u_cpu : cpu
  ASSERT_EQ(soc.children.size(), 1u);
  const auto &cpu = soc.children[0];
  EXPECT_EQ(cpu.instance_name, "u_cpu");
  EXPECT_EQ(cpu.module_type, "cpu");

  // cpu has two children: u_alu_1 : alu, u_alu_2 : alu
  ASSERT_EQ(cpu.children.size(), 2u);
  EXPECT_EQ(cpu.children[0].instance_name, "u_alu_1");
  EXPECT_EQ(cpu.children[0].module_type, "alu");
  EXPECT_TRUE(cpu.children[0].children.empty());

  EXPECT_EQ(cpu.children[1].instance_name, "u_alu_2");
  EXPECT_EQ(cpu.children[1].module_type, "alu");
  EXPECT_TRUE(cpu.children[1].children.empty());
}

TEST(InstanceForestTest, MultipleTopLevelModules) {
  // Two modules, neither instantiates the other.
  const std::string src =
      "module a; endmodule\n"
      "module b; endmodule\n";
  auto forest = ForestFromSource(src);
  ASSERT_EQ(forest.size(), 2u);
  EXPECT_EQ(forest[0].module_type, "a");
  EXPECT_EQ(forest[1].module_type, "b");
}

TEST(InstanceForestTest, UnknownModuleTypeIsLeaf) {
  // a instantiates "unknown_mod" which is not defined in this file.
  const std::string src =
      "module a;\n"
      "  unknown_mod u_um();\n"
      "endmodule\n";
  auto result = ResultFromSource(src);
  const auto &forest = result.forest;
  ASSERT_EQ(forest.size(), 1u);
  const auto &a = forest[0];
  EXPECT_EQ(a.module_type, "a");
  ASSERT_EQ(a.children.size(), 1u);
  EXPECT_EQ(a.children[0].instance_name, "u_um");
  EXPECT_EQ(a.children[0].module_type, "unknown_mod");
  EXPECT_TRUE(a.children[0].children.empty());

  // Should produce an undeclared error.
  EXPECT_FALSE(result.errors.empty());
  bool found = false;
  for (const auto &err : result.errors) {
    if (err.find("undeclared") != std::string::npos &&
        err.find("'unknown_mod'") != std::string::npos &&
        err.find("'u_um'") != std::string::npos) {
      found = true;
    }
  }
  EXPECT_TRUE(found);
}

// Nested module scope fix

TEST(InstanceForestTest, NestedModuleScope) {
  // module a contains nested module "inner" and a direct instantiation of d.
  // Only d should appear under a; c should appear only under inner.
  const std::string src =
      "module a;\n"
      "  module inner;\n"
      "    c u_c();\n"
      "  endmodule\n"
      "  d u_d();\n"
      "endmodule\n";
  auto forest = ForestFromSource(src);

  ASSERT_EQ(forest.size(), 2u);

  // Find each root by name.
  const InstanceNode *a_node = nullptr;
  const InstanceNode *inner_node = nullptr;
  for (const auto &root : forest) {
    if (root.module_type == "a") a_node = &root;
    if (root.module_type == "inner") inner_node = &root;
  }
  ASSERT_NE(a_node, nullptr);
  ASSERT_NE(inner_node, nullptr);

  // a should only have d (not c).
  ASSERT_EQ(a_node->children.size(), 1u);
  EXPECT_EQ(a_node->children[0].module_type, "d");

  // inner should have c.
  ASSERT_EQ(inner_node->children.size(), 1u);
  EXPECT_EQ(inner_node->children[0].module_type, "c");
}

TEST(InstanceForestTest, NestedModuleScopeNoLeakageMultipleLevels) {
  // Two levels of nesting: outer -> middle -> inner.
  // Each module has its own instantiation that should not leak upward.
  const std::string src =
      "module outer;\n"
      "  module middle;\n"
      "    module inner;\n"
      "      x u_x();\n"
      "    endmodule\n"
      "    y u_y();\n"
      "  endmodule\n"
      "  z u_z();\n"
      "endmodule\n";
  auto forest = ForestFromSource(src);

  // All three modules are discovered as roots (nested definitions are not
  // kDataDeclaration instantiations, so they don't count as "instantiated").
  ASSERT_EQ(forest.size(), 3u);

  const InstanceNode *outer_node = nullptr;
  const InstanceNode *middle_node = nullptr;
  const InstanceNode *inner_node = nullptr;
  for (const auto &root : forest) {
    if (root.module_type == "outer") outer_node = &root;
    if (root.module_type == "middle") middle_node = &root;
    if (root.module_type == "inner") inner_node = &root;
  }
  ASSERT_NE(outer_node, nullptr);
  ASSERT_NE(middle_node, nullptr);
  ASSERT_NE(inner_node, nullptr);

  ASSERT_EQ(outer_node->children.size(), 1u);
  EXPECT_EQ(outer_node->children[0].module_type, "z");

  ASSERT_EQ(middle_node->children.size(), 1u);
  EXPECT_EQ(middle_node->children[0].module_type, "y");

  ASSERT_EQ(inner_node->children.size(), 1u);
  EXPECT_EQ(inner_node->children[0].module_type, "x");
}

// Interface support

TEST(InstanceForestTest, StandaloneInterface) {
  // A standalone interface with no instantiations should appear as a root.
  const std::string src = "interface axi_if; endinterface\n";
  auto forest = ForestFromSource(src);
  ASSERT_EQ(forest.size(), 1u);
  EXPECT_EQ(forest[0].module_type, "axi_if");
  EXPECT_TRUE(forest[0].instance_name.empty());
  EXPECT_TRUE(forest[0].children.empty());
}

TEST(InstanceForestTest, InterfaceInstantiatedByModule) {
  // A module instantiates an interface.  The interface should appear as a
  // known type (leaf with no children) rather than an unknown leaf.
  const std::string src =
      "interface axi_if;\n"
      "endinterface\n"
      "module cpu;\n"
      "  axi_if u_bus();\n"
      "endmodule\n";
  auto forest = ForestFromSource(src);

  // cpu is the only top-level root (axi_if is instantiated).
  ASSERT_EQ(forest.size(), 1u);
  EXPECT_EQ(forest[0].module_type, "cpu");

  ASSERT_EQ(forest[0].children.size(), 1u);
  EXPECT_EQ(forest[0].children[0].instance_name, "u_bus");
  EXPECT_EQ(forest[0].children[0].module_type, "axi_if");
  EXPECT_TRUE(forest[0].children[0].children.empty());
}

TEST(InstanceForestTest, InterfaceAndModuleMixed) {
  // Both interfaces and modules in the same file.
  const std::string src =
      "interface axi_if;\n"
      "endinterface\n"
      "module alu; endmodule\n"
      "module cpu;\n"
      "  alu u_alu();\n"
      "  axi_if u_bus();\n"
      "endmodule\n";
  auto forest = ForestFromSource(src);

  // cpu is the only top-level root.
  ASSERT_EQ(forest.size(), 1u);
  EXPECT_EQ(forest[0].module_type, "cpu");

  ASSERT_EQ(forest[0].children.size(), 2u);
  EXPECT_EQ(forest[0].children[0].instance_name, "u_alu");
  EXPECT_EQ(forest[0].children[0].module_type, "alu");
  EXPECT_EQ(forest[0].children[1].instance_name, "u_bus");
  EXPECT_EQ(forest[0].children[1].module_type, "axi_if");
}

// Declaration kind classification (Issue 1)

TEST(DeclarationKindTest, ModuleIsClassifiedAsModule) {
  const std::string src =
      "module alu; endmodule\n"
      "module cpu;\n"
      "  alu u_alu();\n"
      "endmodule\n";
  auto forest = ForestFromSource(src);

  ASSERT_EQ(forest.size(), 1u);
  EXPECT_EQ(forest[0].declaration_kind, DeclarationKind::kModule);
  ASSERT_EQ(forest[0].children.size(), 1u);
  EXPECT_EQ(forest[0].children[0].declaration_kind, DeclarationKind::kModule);
}

TEST(DeclarationKindTest, InterfaceIsClassifiedAsInterface) {
  const std::string src = "interface axi_if; endinterface\n";
  auto forest = ForestFromSource(src);

  ASSERT_EQ(forest.size(), 1u);
  EXPECT_EQ(forest[0].declaration_kind, DeclarationKind::kInterface);
}

TEST(DeclarationKindTest, MixedKindsInHierarchy) {
  const std::string src =
      "interface axi_if; endinterface\n"
      "module cpu;\n"
      "  axi_if u_bus();\n"
      "endmodule\n";
  auto forest = ForestFromSource(src);

  ASSERT_EQ(forest.size(), 1u);
  EXPECT_EQ(forest[0].declaration_kind, DeclarationKind::kModule);
  EXPECT_EQ(forest[0].module_type, "cpu");

  ASSERT_EQ(forest[0].children.size(), 1u);
  EXPECT_EQ(forest[0].children[0].declaration_kind,
            DeclarationKind::kInterface);
  EXPECT_EQ(forest[0].children[0].module_type, "axi_if");
}

TEST(DeclarationKindTest, UndeclaredTypeIsUnknown) {
  const std::string src =
      "module top;\n"
      "  unknown_mod u0();\n"
      "endmodule\n";
  auto forest = ForestFromSource(src);

  ASSERT_EQ(forest.size(), 1u);
  ASSERT_EQ(forest[0].children.size(), 1u);
  EXPECT_EQ(forest[0].children[0].declaration_kind,
            DeclarationKind::kUnknown);
}

// Undeclared module errors (Issue 2)

TEST(UndeclaredModuleTest, ErrorOnUndeclaredModule) {
  const std::string src =
      "module top;\n"
      "  unknown_mod u0();\n"
      "endmodule\n";
  auto result = ResultFromSource(src);
  EXPECT_FALSE(result.errors.empty());

  bool found = false;
  for (const auto &err : result.errors) {
    if (err.find("undeclared") != std::string::npos &&
        err.find("'unknown_mod'") != std::string::npos &&
        err.find("'u0'") != std::string::npos) {
      found = true;
    }
  }
  EXPECT_TRUE(found) << "Expected undeclared module error for 'unknown_mod'";
}

TEST(UndeclaredModuleTest, NoErrorWhenAllDeclared) {
  const std::string src =
      "module alu; endmodule\n"
      "module cpu;\n"
      "  alu u_alu();\n"
      "endmodule\n";
  auto result = ResultFromSource(src);
  EXPECT_TRUE(result.errors.empty());
}

// Multiple declarations error (Issue 3)

TEST(DuplicateDeclarationTest, ErrorOnDuplicateModule) {
  const std::string src =
      "module cpu;\n"
      "  a u_a();\n"
      "endmodule\n"
      "module cpu;\n"
      "  b u_b();\n"
      "endmodule\n";
  auto result = ResultFromSource(src);
  EXPECT_FALSE(result.errors.empty());

  bool found = false;
  for (const auto &err : result.errors) {
    if (err.find("multiple declarations") != std::string::npos &&
        err.find("'cpu'") != std::string::npos) {
      found = true;
    }
  }
  EXPECT_TRUE(found) << "Expected multiple declarations error for 'cpu'";

  // Only the first declaration should be used.
  ASSERT_EQ(result.forest.size(), 1u);
  EXPECT_EQ(result.forest[0].module_type, "cpu");
  ASSERT_EQ(result.forest[0].children.size(), 1u);
  EXPECT_EQ(result.forest[0].children[0].module_type, "a");
}

TEST(DuplicateDeclarationTest, ErrorOnDuplicateInterface) {
  const std::string src =
      "interface bus_if; endinterface\n"
      "interface bus_if; endinterface\n";
  auto result = ResultFromSource(src);
  EXPECT_FALSE(result.errors.empty());

  bool found = false;
  for (const auto &err : result.errors) {
    if (err.find("multiple declarations") != std::string::npos &&
        err.find("'bus_if'") != std::string::npos) {
      found = true;
    }
  }
  EXPECT_TRUE(found);
}

// Parameter type elaboration (Issue 4)

TEST(ParamElaborationTest, DefaultTypeParam) {
  // wrapper declares parameter type T = alu; T u_inst().
  // Without override, u_inst should resolve to alu.
  // Note: 'alu' appears as a separate top-level root because no module
  // directly instantiates it (only referenced via parameter default).
  const std::string src =
      "module alu; endmodule\n"
      "module wrapper #(parameter type T = alu);\n"
      "  T u_inst();\n"
      "endmodule\n"
      "module top;\n"
      "  wrapper u_w();\n"
      "endmodule\n";
  auto result = ResultFromSource(src);
  const auto &forest = result.forest;

  // Find 'top' in the forest (alu is also a root since it's never
  // directly instantiated).
  const InstanceNode *top_node = nullptr;
  for (const auto &root : forest) {
    if (root.module_type == "top") {
      top_node = &root;
      break;
    }
  }
  ASSERT_NE(top_node, nullptr);

  // top -> u_w : wrapper
  ASSERT_EQ(top_node->children.size(), 1u);
  const auto &wrapper = top_node->children[0];
  EXPECT_EQ(wrapper.module_type, "wrapper");

  // wrapper -> u_inst : alu (resolved from T's default)
  ASSERT_EQ(wrapper.children.size(), 1u);
  EXPECT_EQ(wrapper.children[0].instance_name, "u_inst");
  EXPECT_EQ(wrapper.children[0].module_type, "alu");
}

TEST(ParamElaborationTest, OverrideTypeParam) {
  // wrapper declares parameter type T = alu; T u_inst().
  // top instantiates wrapper #(.T(cpu)), so u_inst should resolve to cpu.
  // Note: 'alu' and 'cpu' appear as separate top-level roots because no
  // module directly instantiates them.
  const std::string src =
      "module alu; endmodule\n"
      "module cpu; endmodule\n"
      "module wrapper #(parameter type T = alu);\n"
      "  T u_inst();\n"
      "endmodule\n"
      "module top;\n"
      "  wrapper #(.T(cpu)) u_w();\n"
      "endmodule\n";
  auto result = ResultFromSource(src);
  const auto &forest = result.forest;

  // Find 'top' in the forest.
  const InstanceNode *top_node = nullptr;
  for (const auto &root : forest) {
    if (root.module_type == "top") {
      top_node = &root;
      break;
    }
  }
  ASSERT_NE(top_node, nullptr);

  ASSERT_EQ(top_node->children.size(), 1u);
  const auto &wrapper = top_node->children[0];
  EXPECT_EQ(wrapper.module_type, "wrapper");

  // wrapper -> u_inst : cpu (overridden from T's default alu)
  ASSERT_EQ(wrapper.children.size(), 1u);
  EXPECT_EQ(wrapper.children[0].instance_name, "u_inst");
  EXPECT_EQ(wrapper.children[0].module_type, "cpu");
}

TEST(ParamElaborationTest, NonTypeParamUnaffected) {
  // A regular value parameter should not affect type resolution.
  const std::string src =
      "module alu; endmodule\n"
      "module cpu #(parameter int WIDTH = 8);\n"
      "  alu u_alu();\n"
      "endmodule\n";
  auto result = ResultFromSource(src);
  ASSERT_EQ(result.forest.size(), 1u);
  EXPECT_EQ(result.forest[0].module_type, "cpu");
  ASSERT_EQ(result.forest[0].children.size(), 1u);
  EXPECT_EQ(result.forest[0].children[0].module_type, "alu");
  EXPECT_TRUE(result.errors.empty());
}

// Chained parameter resolution

TEST(ChainedResolutionTest, TwoLevelChain) {
  // T defaults to U, U defaults to cpu.
  // T should resolve to cpu through the chain T -> U -> cpu.
  const std::string src =
      "module cpu; endmodule\n"
      "module wrapper #(parameter type U = cpu, parameter type T = U);\n"
      "  T u0();\n"
      "endmodule\n"
      "module top;\n"
      "  wrapper u_w();\n"
      "endmodule\n";
  auto result = ResultFromSource(src);
  const auto &forest = result.forest;

  const InstanceNode *top_node = nullptr;
  for (const auto &root : forest) {
    if (root.module_type == "top") {
      top_node = &root;
      break;
    }
  }
  ASSERT_NE(top_node, nullptr);

  // top -> u_w : wrapper -> u0 : cpu (resolved through T -> U -> cpu)
  ASSERT_EQ(top_node->children.size(), 1u);
  const auto &wrapper = top_node->children[0];
  EXPECT_EQ(wrapper.module_type, "wrapper");
  ASSERT_EQ(wrapper.children.size(), 1u);
  EXPECT_EQ(wrapper.children[0].instance_name, "u0");
  EXPECT_EQ(wrapper.children[0].module_type, "cpu");
}

TEST(ChainedResolutionTest, ThreeLevelChain) {
  // V -> U -> T -> cpu: three levels of indirection.
  const std::string src =
      "module cpu; endmodule\n"
      "module wrapper #(\n"
      "  parameter type T = cpu,\n"
      "  parameter type U = T,\n"
      "  parameter type V = U\n"
      ");\n"
      "  V u0();\n"
      "endmodule\n"
      "module top;\n"
      "  wrapper u_w();\n"
      "endmodule\n";
  auto result = ResultFromSource(src);
  const auto &forest = result.forest;

  const InstanceNode *top_node = nullptr;
  for (const auto &root : forest) {
    if (root.module_type == "top") {
      top_node = &root;
      break;
    }
  }
  ASSERT_NE(top_node, nullptr);

  ASSERT_EQ(top_node->children.size(), 1u);
  const auto &wrapper = top_node->children[0];
  ASSERT_EQ(wrapper.children.size(), 1u);
  EXPECT_EQ(wrapper.children[0].module_type, "cpu");
}

TEST(ChainedResolutionTest, CycleDetection) {
  // T -> U -> T: cycle. Should not infinite-loop.
  // The resolved type will be one of {T, U} — the important thing is no crash.
  const std::string src =
      "module wrapper #(parameter type T = U, parameter type U = T);\n"
      "  T u0();\n"
      "endmodule\n";
  auto result = ResultFromSource(src);
  // Should not crash. Forest should have wrapper as root.
  ASSERT_GE(result.forest.size(), 1u);
  const InstanceNode *wrapper_node = nullptr;
  for (const auto &root : result.forest) {
    if (root.module_type == "wrapper") {
      wrapper_node = &root;
      break;
    }
  }
  ASSERT_NE(wrapper_node, nullptr);
  // u0 should exist but its module_type will be an unresolved name
  // (the cycle can't be fully resolved).
  ASSERT_EQ(wrapper_node->children.size(), 1u);
  EXPECT_EQ(wrapper_node->children[0].instance_name, "u0");
}

TEST(ChainedResolutionTest, OverrideBreaksChain) {
  // T defaults to U, U defaults to alu.
  // Instantiation overrides T directly to cpu.
  // Should resolve to cpu, not follow the default chain.
  const std::string src =
      "module alu; endmodule\n"
      "module cpu; endmodule\n"
      "module wrapper #(parameter type U = alu, parameter type T = U);\n"
      "  T u0();\n"
      "endmodule\n"
      "module top;\n"
      "  wrapper #(.T(cpu)) u_w();\n"
      "endmodule\n";
  auto result = ResultFromSource(src);
  const auto &forest = result.forest;

  const InstanceNode *top_node = nullptr;
  for (const auto &root : forest) {
    if (root.module_type == "top") {
      top_node = &root;
      break;
    }
  }
  ASSERT_NE(top_node, nullptr);

  ASSERT_EQ(top_node->children.size(), 1u);
  const auto &wrapper = top_node->children[0];
  ASSERT_EQ(wrapper.children.size(), 1u);
  // T overridden to cpu directly, so it should be cpu, not alu.
  EXPECT_EQ(wrapper.children[0].module_type, "cpu");
}

// Full type expression preservation

TEST(FullTypeExprTest, PackageQualifiedDefault) {
  // parameter type T = pkg::cpu -> should preserve "pkg::cpu" not just "pkg".
  const std::string src =
      "module wrapper #(parameter type T = pkg::cpu);\n"
      "  T u0();\n"
      "endmodule\n";
  auto result = ResultFromSource(src);
  ASSERT_GE(result.forest.size(), 1u);
  const InstanceNode *wrapper = nullptr;
  for (const auto &root : result.forest) {
    if (root.module_type == "wrapper") {
      wrapper = &root;
      break;
    }
  }
  ASSERT_NE(wrapper, nullptr);
  ASSERT_EQ(wrapper->children.size(), 1u);
  // Should be "pkg::cpu", not "pkg".
  EXPECT_EQ(wrapper->children[0].module_type, "pkg::cpu");
}

TEST(FullTypeExprTest, ParameterizedDefault) {
  // parameter type T = Foo#(int) -> base type is "Foo" (params stripped).
  const std::string src =
      "module wrapper #(parameter type T = Foo#(int));\n"
      "  T u0();\n"
      "endmodule\n";
  auto result = ResultFromSource(src);
  ASSERT_GE(result.forest.size(), 1u);
  const InstanceNode *wrapper = nullptr;
  for (const auto &root : result.forest) {
    if (root.module_type == "wrapper") {
      wrapper = &root;
      break;
    }
  }
  ASSERT_NE(wrapper, nullptr);
  ASSERT_EQ(wrapper->children.size(), 1u);
  EXPECT_EQ(wrapper->children[0].module_type, "Foo");
}

TEST(FullTypeExprTest, PackageQualifiedParameterized) {
  // parameter type T = pkg::Foo#(int) -> base type is "pkg::Foo".
  const std::string src =
      "module wrapper #(parameter type T = pkg::Foo#(int));\n"
      "  T u0();\n"
      "endmodule\n";
  auto result = ResultFromSource(src);
  ASSERT_GE(result.forest.size(), 1u);
  const InstanceNode *wrapper = nullptr;
  for (const auto &root : result.forest) {
    if (root.module_type == "wrapper") {
      wrapper = &root;
      break;
    }
  }
  ASSERT_NE(wrapper, nullptr);
  ASSERT_EQ(wrapper->children.size(), 1u);
  EXPECT_EQ(wrapper->children[0].module_type, "pkg::Foo");
}

TEST(FullTypeExprTest, PackageQualifiedOverride) {
  // Override with a package-qualified name: #(.T(pkg::cpu))
  const std::string src =
      "module alu; endmodule\n"
      "module wrapper #(parameter type T = alu);\n"
      "  T u0();\n"
      "endmodule\n"
      "module top;\n"
      "  wrapper #(.T(pkg::cpu)) u_w();\n"
      "endmodule\n";
  auto result = ResultFromSource(src);
  const auto &forest = result.forest;

  const InstanceNode *top_node = nullptr;
  for (const auto &root : forest) {
    if (root.module_type == "top") {
      top_node = &root;
      break;
    }
  }
  ASSERT_NE(top_node, nullptr);
  ASSERT_EQ(top_node->children.size(), 1u);
  const auto &wrapper = top_node->children[0];
  ASSERT_EQ(wrapper.children.size(), 1u);
  EXPECT_EQ(wrapper.children[0].module_type, "pkg::cpu");
}

TEST(FullTypeExprTest, ParameterizedLookupMatchesDeclMap) {
  // Foo#(4) should resolve to base type "Foo" which matches the
  // declaration of module Foo.  No undeclared errors.
  const std::string src =
      "module Foo #(parameter int N = 0); endmodule\n"
      "module wrapper #(parameter type T = Foo#(4));\n"
      "  T u0();\n"
      "endmodule\n";
  auto result = ResultFromSource(src);
  ASSERT_GE(result.forest.size(), 1u);
  const InstanceNode *wrapper = nullptr;
  for (const auto &root : result.forest) {
    if (root.module_type == "wrapper") {
      wrapper = &root;
      break;
    }
  }
  ASSERT_NE(wrapper, nullptr);
  ASSERT_EQ(wrapper->children.size(), 1u);
  // Should resolve to "Foo" (matching the module declaration), not "Foo#(4)".
  EXPECT_EQ(wrapper->children[0].module_type, "Foo");
  EXPECT_EQ(wrapper->children[0].declaration_kind, DeclarationKind::kModule);
  // No undeclared errors since "Foo" IS declared.
  bool has_undeclared = false;
  for (const auto &err : result.errors) {
    if (err.find("undeclared") != std::string::npos &&
        err.find("Foo") != std::string::npos) {
      has_undeclared = true;
    }
  }
  EXPECT_FALSE(has_undeclared);
}

// PrintHierarchyTree tests

TEST(PrintHierarchyTreeTest, SingleModuleWithKind) {
  const std::string src = "module top; endmodule\n";
  auto forest = ForestFromSource(src);
  std::string output = PrintHierarchyTree(forest);
  EXPECT_EQ(output, "module top\n");
}

TEST(PrintHierarchyTreeTest, SingleInterfaceWithKind) {
  const std::string src = "interface axi_if; endinterface\n";
  auto forest = ForestFromSource(src);
  std::string output = PrintHierarchyTree(forest);
  EXPECT_EQ(output, "interface axi_if\n");
}

TEST(PrintHierarchyTreeTest, MixedHierarchyLabels) {
  const std::string src =
      "interface axi_if; endinterface\n"
      "module alu; endmodule\n"
      "module cpu;\n"
      "  alu u_alu();\n"
      "  axi_if u_bus();\n"
      "endmodule\n";
  auto forest = ForestFromSource(src);
  std::string output = PrintHierarchyTree(forest);

  // Verify key elements are present.
  EXPECT_NE(output.find("module cpu"), std::string::npos);
  EXPECT_NE(output.find("u_alu : module alu"), std::string::npos);
  EXPECT_NE(output.find("u_bus : interface axi_if"), std::string::npos);
}

TEST(PrintHierarchyTreeTest, AluCpuSocOutput) {
  const std::string src =
      "module alu; endmodule\n"
      "module cpu;\n"
      "  alu u_alu_1();\n"
      "  alu u_alu_2();\n"
      "endmodule\n"
      "module soc;\n"
      "  cpu u_cpu();\n"
      "endmodule\n";
  auto forest = ForestFromSource(src);
  std::string output = PrintHierarchyTree(forest);

  // Expected output:
  // module soc
  // └── u_cpu : module cpu
  //     ├── u_alu_1 : module alu
  //     └── u_alu_2 : module alu
  EXPECT_NE(output.find("module soc"), std::string::npos);
  EXPECT_NE(output.find("u_cpu : module cpu"), std::string::npos);
  EXPECT_NE(output.find("u_alu_1 : module alu"), std::string::npos);
  EXPECT_NE(output.find("u_alu_2 : module alu"), std::string::npos);
}

// Multi-file tests (CollectDeclarations + BuildInstanceForestFromMaps)

// Helper: parse multiple "files" (source strings), collect declarations from
// each, merge the maps, and build the combined forest.
// Keeps the analyzers alive so CST pointers in decl_map remain valid.
static BuildResult MultiFileBuild(
    const std::vector<std::string> &sources) {
  std::vector<std::unique_ptr<verilog::VerilogAnalyzer>> analyzers;
  std::vector<FileDeclarations> per_file;

  for (const auto &src : sources) {
    auto analyzer = verilog::VerilogAnalyzer::AnalyzeAutomaticMode(
        src, "<test>", kDefaultPreprocess);
    if (analyzer == nullptr) continue;
    if (!analyzer->LexStatus().ok()) continue;
    if (!analyzer->ParseStatus().ok()) continue;
    const auto &tree = analyzer->SyntaxTree();
    if (tree == nullptr) {
      analyzers.push_back(std::move(analyzer));
      continue;
    }
    per_file.push_back(CollectDeclarations(*tree));
    analyzers.push_back(std::move(analyzer));
  }

  // Merge maps.
  std::map<std::string, std::vector<InstantiationRecord>> global_module_map;
  std::map<std::string, DeclarationKind> global_kind_map;
  std::map<std::string, const verible::Symbol *> global_decl_map;
  std::vector<std::string> global_all_modules;
  std::vector<std::string> merge_errors;

  for (const auto &decls : per_file) {
    // Include per-file collection errors.
    for (const auto &err : decls.errors) {
      merge_errors.push_back(err);
    }
    for (const auto &name : decls.all_modules) {
      if (global_module_map.count(name)) {
        merge_errors.push_back(
            "error: multiple declarations of module/interface '" + name + "'");
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

  auto result = BuildInstanceForestFromMaps(
      global_module_map, global_kind_map, global_decl_map, global_all_modules);
  // Prepend merge errors.
  result.errors.insert(result.errors.begin(),
                       merge_errors.begin(), merge_errors.end());
  return result;
}

TEST(MultiFileTest, CrossFileParamOverride) {
  // File 1: module cpu
  // File 2: module wrapper #(parameter type T = cpu); T u0();
  // File 3: module top; wrapper #(.T(cpu)) w();
  // Expected: top -> w : wrapper -> u0 : cpu (resolved across files)
  auto result = MultiFileBuild({
      "module cpu; endmodule",
      "module wrapper #(parameter type T = cpu);\n"
      "  T u0();\n"
      "endmodule",
      "module top;\n"
      "  wrapper #(.T(cpu)) w();\n"
      "endmodule",
  });

  const InstanceNode *top_node = nullptr;
  for (const auto &root : result.forest) {
    if (root.module_type == "top") {
      top_node = &root;
      break;
    }
  }
  ASSERT_NE(top_node, nullptr);
  ASSERT_EQ(top_node->children.size(), 1u);
  const auto &wrapper = top_node->children[0];
  EXPECT_EQ(wrapper.module_type, "wrapper");
  ASSERT_EQ(wrapper.children.size(), 1u);
  EXPECT_EQ(wrapper.children[0].instance_name, "u0");
  EXPECT_EQ(wrapper.children[0].module_type, "cpu");
  EXPECT_EQ(wrapper.children[0].declaration_kind, DeclarationKind::kModule);

  // No undeclared errors — all modules are known across files.
  for (const auto &err : result.errors) {
    EXPECT_EQ(err.find("undeclared"), std::string::npos) << err;
  }
}

TEST(MultiFileTest, ReversedFileOrdering) {
  // Same modules as CrossFileParamOverride but files provided in reverse order.
  // The hierarchy should be identical regardless of file order.
  auto result = MultiFileBuild({
      "module top;\n"
      "  wrapper #(.T(cpu)) w();\n"
      "endmodule",
      "module wrapper #(parameter type T = cpu);\n"
      "  T u0();\n"
      "endmodule",
      "module cpu; endmodule",
  });

  const InstanceNode *top_node = nullptr;
  for (const auto &root : result.forest) {
    if (root.module_type == "top") {
      top_node = &root;
      break;
    }
  }
  ASSERT_NE(top_node, nullptr);
  ASSERT_EQ(top_node->children.size(), 1u);
  const auto &wrapper = top_node->children[0];
  EXPECT_EQ(wrapper.module_type, "wrapper");
  ASSERT_EQ(wrapper.children.size(), 1u);
  EXPECT_EQ(wrapper.children[0].module_type, "cpu");

  // No undeclared errors.
  for (const auto &err : result.errors) {
    EXPECT_EQ(err.find("undeclared"), std::string::npos) << err;
  }
}

TEST(MultiFileTest, DuplicateDeclarationsAcrossFiles) {
  // cpu is declared in two separate files — should produce a duplicate error.
  auto result = MultiFileBuild({
      "module cpu; endmodule",
      "module cpu;\n"
      "  alu u_alu();\n"
      "endmodule",
  });

  bool found_dup = false;
  for (const auto &err : result.errors) {
    if (err.find("multiple declarations") != std::string::npos &&
        err.find("'cpu'") != std::string::npos) {
      found_dup = true;
    }
  }
  EXPECT_TRUE(found_dup) << "Expected duplicate declaration error for 'cpu'";

  // First declaration should win.
  ASSERT_GE(result.forest.size(), 1u);
  const InstanceNode *cpu_node = nullptr;
  for (const auto &root : result.forest) {
    if (root.module_type == "cpu") {
      cpu_node = &root;
      break;
    }
  }
  ASSERT_NE(cpu_node, nullptr);
  // First cpu has no children.
  EXPECT_TRUE(cpu_node->children.empty());
}

TEST(MultiFileTest, InterfaceDeclarationsAcrossFiles) {
  // File 1: interface axi_if
  // File 2: module cpu; axi_if u_bus();
  // cpu should see axi_if as a known interface type.
  auto result = MultiFileBuild({
      "interface axi_if; endinterface",
      "module cpu;\n"
      "  axi_if u_bus();\n"
      "endmodule",
  });

  const InstanceNode *cpu_node = nullptr;
  for (const auto &root : result.forest) {
    if (root.module_type == "cpu") {
      cpu_node = &root;
      break;
    }
  }
  ASSERT_NE(cpu_node, nullptr);
  EXPECT_EQ(cpu_node->declaration_kind, DeclarationKind::kModule);
  ASSERT_EQ(cpu_node->children.size(), 1u);
  EXPECT_EQ(cpu_node->children[0].instance_name, "u_bus");
  EXPECT_EQ(cpu_node->children[0].module_type, "axi_if");
  EXPECT_EQ(cpu_node->children[0].declaration_kind,
            DeclarationKind::kInterface);

  // No undeclared errors.
  for (const auto &err : result.errors) {
    EXPECT_EQ(err.find("undeclared"), std::string::npos) << err;
  }
}

TEST(MultiFileTest, CrossFileDefaultResolution) {
  // File 1: module cpu
  // File 2: module wrapper #(parameter type T = cpu); T u0();
  // File 3: module top; wrapper w();   (no override — uses default T=cpu)
  // Expected: top -> w : wrapper -> u0 : cpu (default resolved across files)
  auto result = MultiFileBuild({
      "module cpu; endmodule",
      "module wrapper #(parameter type T = cpu);\n"
      "  T u0();\n"
      "endmodule",
      "module top;\n"
      "  wrapper w();\n"
      "endmodule",
  });

  const InstanceNode *top_node = nullptr;
  for (const auto &root : result.forest) {
    if (root.module_type == "top") {
      top_node = &root;
      break;
    }
  }
  ASSERT_NE(top_node, nullptr);
  ASSERT_EQ(top_node->children.size(), 1u);
  const auto &wrapper = top_node->children[0];
  EXPECT_EQ(wrapper.module_type, "wrapper");
  ASSERT_EQ(wrapper.children.size(), 1u);
  EXPECT_EQ(wrapper.children[0].module_type, "cpu");

  // Verify the tree output matches the expected format.
  std::string output = PrintHierarchyTree(result.forest);
  EXPECT_NE(output.find("module top"), std::string::npos);
  EXPECT_NE(output.find("w : module wrapper"), std::string::npos);
  EXPECT_NE(output.find("u0 : module cpu"), std::string::npos);
}

}  // namespace
}  // namespace analysis
}  // namespace verilog

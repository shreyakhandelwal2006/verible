#include "verible/verilog/analysis/module-hierarchy.h"

#include "gtest/gtest.h"
#include "verible/verilog/analysis/verilog-analyzer.h"
#include "verible/verilog/preprocessor/verilog-preprocess.h"

namespace verilog {
namespace analysis {
namespace {

static constexpr VerilogPreprocess::Config kDefaultPreprocess;

static std::vector<InstanceNode> ForestFromSource(const std::string &source) {
  auto analyzer = verilog::VerilogAnalyzer::AnalyzeAutomaticMode(
      source, "<test>", kDefaultPreprocess);
  if (analyzer == nullptr) return {};
  if (!analyzer->LexStatus().ok()) return {};
  if (!analyzer->ParseStatus().ok()) return {};
  const auto &tree = analyzer->SyntaxTree();
  if (tree == nullptr) return {};
  return BuildInstanceForest(*tree);
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
  auto forest = ForestFromSource(src);

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
  auto forest = ForestFromSource(src);
  ASSERT_EQ(forest.size(), 1u);
  const auto &a = forest[0];
  EXPECT_EQ(a.module_type, "a");
  ASSERT_EQ(a.children.size(), 1u);
  EXPECT_EQ(a.children[0].instance_name, "u_um");
  EXPECT_EQ(a.children[0].module_type, "unknown_mod");
  EXPECT_TRUE(a.children[0].children.empty());
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

}  // namespace
}  // namespace analysis
}  // namespace verilog

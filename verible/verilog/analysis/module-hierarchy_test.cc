#include "verible/verilog/analysis/module-hierarchy.h"

#include "gtest/gtest.h"
#include "verible/verilog/analysis/verilog-analyzer.h"
#include "verible/verilog/preprocessor/verilog-preprocess.h"

namespace verilog {
namespace analysis {
namespace {

static constexpr VerilogPreprocess::Config kDefaultPreprocess;

static ModuleHierarchyMap HierarchyFromSource(const std::string &source) {
  auto analyzer = verilog::VerilogAnalyzer::AnalyzeAutomaticMode(
      source, "<test>", kDefaultPreprocess);
  if (analyzer == nullptr) return {};
  if (!analyzer->LexStatus().ok()) return {};
  if (!analyzer->ParseStatus().ok()) return {};
  const auto &tree = analyzer->SyntaxTree();
  if (tree == nullptr) return {};
  return BuildModuleHierarchy(*tree);
}

TEST(ModuleHierarchyTest, EmptySource) {
  auto h = HierarchyFromSource("");
  EXPECT_TRUE(h.empty());
}

TEST(ModuleHierarchyTest, SingleModuleNoInstantiations) {
  const std::string src = "module a; endmodule\n";
  auto h = HierarchyFromSource(src);
  ASSERT_EQ(h.size(), 1u);
  EXPECT_TRUE(h.count("a"));
  EXPECT_TRUE(h.at("a").empty());
}

TEST(ModuleHierarchyTest, TwoModulesNoInstantiations) {
  const std::string src =
      "module b; endmodule\n"
      "module a; endmodule\n";
  auto h = HierarchyFromSource(src);
  ASSERT_EQ(h.size(), 2u);
  EXPECT_TRUE(h.count("b"));
  EXPECT_TRUE(h.count("a"));
  EXPECT_TRUE(h.at("b").empty());
  EXPECT_TRUE(h.at("a").empty());
}

TEST(ModuleHierarchyTest, SimpleParentChild) {
  // a instantiates b once.
  const std::string src =
      "module b; endmodule\n"
      "module a;\n"
      "  b u_b();\n"
      "endmodule\n";
  auto h = HierarchyFromSource(src);
  ASSERT_TRUE(h.count("b"));
  ASSERT_TRUE(h.count("a"));
  EXPECT_TRUE(h.at("b").empty());
  ASSERT_EQ(h.at("a").size(), 1u);
  EXPECT_EQ(h.at("a")[0], "b");
}

TEST(ModuleHierarchyTest, MultipleInstancesSameType) {
  // a instantiates b twice in one line.
  const std::string src =
      "module b; endmodule\n"
      "module a;\n"
      "  b u1(), u2();\n"
      "endmodule\n";
  auto h = HierarchyFromSource(src);
  ASSERT_TRUE(h.count("a"));
  // Two gate instances -> two entries.
  ASSERT_EQ(h.at("a").size(), 2u);
  EXPECT_EQ(h.at("a")[0], "b");
  EXPECT_EQ(h.at("a")[1], "b");
}

TEST(ModuleHierarchyTest, TwoDifferentInstantiations) {
  const std::string src =
      "module b; endmodule\n"
      "module reg_file; endmodule\n"
      "module a;\n"
      "  b     u_b();\n"
      "  c     u_c();\n"
      "endmodule\n";
  auto h = HierarchyFromSource(src);
  ASSERT_TRUE(h.count("a"));
  const auto& children = h.at("a");
  ASSERT_EQ(children.size(), 2u);
  // Order is declaration order.
  EXPECT_EQ(children[0], "b");
  EXPECT_EQ(children[1], "c");
}

TEST(ModuleHierarchyTest, MultiLevelHierarchy) {
  // a -> b -> c
  const std::string src =
      "module c; endmodule\n"
      "module b;\n"
      "  c u_c();\n"
      "endmodule\n"
      "module a;\n"
      "  b u_b();\n"
      "endmodule\n";
  auto h = HierarchyFromSource(src);
  ASSERT_EQ(h.size(), 3u);

  EXPECT_TRUE(h.at("c").empty());

  ASSERT_EQ(h.at("b").size(), 1u);
  EXPECT_EQ(h.at("b")[0], "c");

  ASSERT_EQ(h.at("a").size(), 1u);
  EXPECT_EQ(h.at("a")[0], "b");
}

TEST(ModuleHierarchyTest, VariableDeclarationNotCounted) {
  // A plain variable declaration must NOT be counted as a module instantiation.
  const std::string src =
      "module a;\n"
      "  logic clk;\n"
      "  wire [7:0] data;\n"
      "endmodule\n";
  auto h = HierarchyFromSource(src);
  ASSERT_TRUE(h.count("a"));
  EXPECT_TRUE(h.at("a").empty());
}

TEST(ModuleHierarchyTest, InstantiationWithParameters) {
  // Parameterised instantiation: b #(.WIDTH(8)) u1();
  const std::string src =
      "module b #(parameter WIDTH=8); endmodule\n"
      "module a;\n"
      "  b #(.WIDTH(8)) u1();\n"
      "endmodule\n";
  auto h = HierarchyFromSource(src);
  ASSERT_TRUE(h.count("a"));
  ASSERT_EQ(h.at("a").size(), 1u);
  EXPECT_EQ(h.at("a")[0], "b");
}

}  // namespace
}  // namespace analysis
}  // namespace verilog

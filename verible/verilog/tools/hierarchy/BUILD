# 'verible-verilog-hierarchy' extracts and displays module instantiation
# hierarchies from Verilog/SystemVerilog source files.

load("@rules_cc//cc:defs.bzl", "cc_binary")
load("//bazel:variables.bzl", "STATIC_EXECUTABLES_FEATURE")

package(
    default_applicable_licenses = ["//:license"],
    default_visibility = ["//visibility:private"],
    features = ["layering_check"],
)

cc_binary(
    name = "verible-verilog-hierarchy",
    srcs = ["verilog-hierarchy.cc"],
    features = STATIC_EXECUTABLES_FEATURE,
    visibility = ["//visibility:public"],
    deps = [
        "//verible/common/strings:mem-block",
        "//verible/common/util:file-util",
        "//verible/common/util:init-command-line",
        "//verible/common/util:iterator-range",
        "//verible/verilog/analysis:module-hierarchy",
        "//verible/verilog/analysis:verilog-analyzer",
        "//verible/verilog/preprocessor:verilog-preprocess",
        "@abseil-cpp//absl/status",
        "@abseil-cpp//absl/strings",
    ],
)

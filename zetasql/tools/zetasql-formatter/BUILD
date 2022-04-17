package(
    default_visibility = ["//zetasql/base:zetasql_implementation"],
)

cc_binary(
    name = "format",
    srcs = ["format.cc"],
    deps = [
        "//zetasql/public:sql_formatter",
        "@com_google_absl//absl/strings",
        "@com_google_absl//absl/types:optional",
        "@com_github_gflags_gflags//:gflags",
    ],
)

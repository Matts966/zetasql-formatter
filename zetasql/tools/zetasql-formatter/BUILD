package(
    default_visibility = ["//zetasql/base:zetasql_implementation"],
)

cc_binary(
    name = "format",
    srcs = ["format.cc"],
    deps = [
        "//zetasql/public:sql_formatter",
        "@com_google_absl//absl/flags:flag",
        "@com_google_absl//absl/flags:parse",
        "@com_google_absl//absl/strings",
        "@com_google_absl//absl/types:optional",
    ],
)

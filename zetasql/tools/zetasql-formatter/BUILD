package(
    default_visibility = ["//zetasql/base:zetasql_implementation"],
)

genrule(
    name = "gen_version",
    outs = ["version.h"],
    cmd = """
TAG=$$(sed -n 's/STABLE_BUILD_GIT_DESCRIBE //p' bazel-out/stable-status.txt)
cat > $@ <<EOF
constexpr char ZSQL_FMT_VERSION_STRING[] = "$$TAG";
EOF
    """,
    stamp = True,
)

cc_library(
    name = "version",
    hdrs = ["version.h"],
)

cc_binary(
    name = "format",
    srcs = ["format.cc"],
    deps = [
        ":version",
        "//zetasql/public:sql_formatter",
        "@com_google_absl//absl/strings",
        "@com_google_absl//absl/types:optional",
        "@com_github_gflags_gflags//:gflags",
    ],
)

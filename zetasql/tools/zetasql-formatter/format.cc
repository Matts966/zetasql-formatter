#include <iostream>
#include <string>
#include <vector>
#include <fstream>
#include <filesystem>

#include "zetasql/base/logging.h"
#include "zetasql/base/status.h"
#include "zetasql/public/sql_formatter.h"
#include "absl/strings/strip.h"
#include "absl/strings/str_join.h"
#include "gflags/gflags.h"

int format(const std::filesystem::path& file_path) {
  std::string formatted;
  if (file_path.extension() == ".bq" || file_path.extension() == ".sql") {
    std::cout << "formatting " << file_path << "..." << std::endl;
    std::ifstream file(file_path, std::ios::in);
    std::string sql(std::istreambuf_iterator<char>(file), {});
    const absl::Status status = zetasql::FormatSql(sql, &formatted);
    if (status.ok()) {
      std::ofstream out(file_path);
      out << formatted;
      if (formatted != sql) {
        std::cout << "successfully formatted " << file_path << "!" << std::endl;
        return 1;
      }
    } else {
      std::cout << "ERROR: " << status << std::endl;
      return 1;
    }
    std::cout << file_path << " is already formatted!" << std::endl;
  }
  return 0;
}

// format formats all sql files in specified directory and returns code 0
// if all files are formatted and 1 if error occurs or any file is formatted.
int main(int argc, char* argv[]) {
  gflags::SetUsageMessage("Usage: zetasql-formatter <paths...>");
  gflags::SetVersionString(VERSION_STRING);
  gflags::ParseCommandLineFlags(&argc, &argv, true);
  if (argc <= 1) {
    std::cerr << kUsage;
    return 1;
  }
  int rc = 0;
  for (;argc--; argv+) {
    if (std::filesystem::is_regular_file(*argv)) {
      std::filesystem::path file_path(*argv);
      return format(file_path);
    }
    std::filesystem::recursive_directory_iterator file_path(*argv,
                                                  std::filesystem::directory_options::skip_permission_denied)
                                                  , end;
    std::error_code err;
    for (; file_path != end; file_path.increment(err)) {
      if (err) {
        std::cout << "WARNING: " << err << std::endl;
      }
      rc |= format(file_path->path());
    }
  }
  return rc;
}

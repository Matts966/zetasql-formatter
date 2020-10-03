//
// Copyright 2019 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

#include "zetasql/public/sql_formatter.h"

#include <memory>
#include <vector>
#include <deque>

#include "zetasql/base/logging.h"
#include "zetasql/parser/parse_tree.h"
#include "zetasql/parser/parser.h"
#include "zetasql/public/error_helpers.h"
#include "zetasql/public/options.pb.h"
#include "zetasql/public/parse_location.h"
#include "zetasql/public/parse_resume_location.h"
#include "zetasql/public/parse_tokens.h"
#include "absl/strings/ascii.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_join.h"
#include "zetasql/base/source_location.h"
#include "zetasql/base/ret_check.h"
#include "zetasql/base/status.h"
#include "zetasql/base/status_builder.h"

namespace zetasql {

absl::Status FormatSql(const std::string& sql, std::string* formatted_sql) {
  ZETASQL_RET_CHECK_NE(formatted_sql, nullptr);
  formatted_sql->clear();

  *formatted_sql = sql;

  ParseTokenOptions options;
  options.include_comments = true;

  std::unique_ptr<ParserOutput> parser_output;
  const auto parser_options = ParserOptions();

  ZETASQL_RETURN_IF_ERROR(ParseScript(sql, parser_options,
                          ErrorMessageMode::ERROR_MESSAGE_MULTI_LINE_WITH_CARET, &parser_output));
  std::deque<std::pair<std::string, ParseLocationPoint>> comments;
  std::vector<ParseToken> parse_tokens;
  bool last_token_is_comment = false;
  ParseResumeLocation location = ParseResumeLocation::FromStringView(sql);
  const absl::Status token_status =
      GetParseTokens(options, &location, &parse_tokens);
  if (token_status.ok()) {
    for (const auto& parse_token : parse_tokens) {
      if (parse_token.IsEndOfInput()) break;
      last_token_is_comment = false;
      if (parse_token.IsComment()) {
        last_token_is_comment = true;
        comments.push_back(std::make_pair(parse_token.GetSQL(), parse_token.GetLocationRange().start()));
      }
    }
    *formatted_sql = UnparseWithComments(parser_output->statement(), comments);
  } else {
    // If GetParseTokens fails, just ignores comments.
    *formatted_sql = Unparse(parser_output->statement());
  }

  // The result from Unparse always ends with '\n'. Strips whitespaces so ';'
  // can follow the statement immediately rather than starting a new line.
  absl::StripAsciiWhitespace(formatted_sql);

  std::string suffix;
  if (last_token_is_comment) {
    suffix = "\n";
  } else {
    suffix = ";\n";
  }
  *formatted_sql = absl::StrCat(*formatted_sql, suffix);
  
  for (const auto& comment : comments) {
    *formatted_sql = absl::StrCat(*formatted_sql, comment.first);
  }
  return absl::OkStatus();
}

}  // namespace zetasql

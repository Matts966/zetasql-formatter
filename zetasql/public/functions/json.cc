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

#include "zetasql/public/functions/json.h"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "zetasql/common/errors.h"
#include "zetasql/public/functions/json_internal.h"
#include "zetasql/public/json_value.h"
#include "absl/memory/memory.h"
#include "absl/status/status.h"
#include "zetasql/base/statusor.h"
#include "absl/strings/match.h"
#include "re2/re2.h"
#include "zetasql/base/status_macros.h"

namespace zetasql {
namespace functions {
namespace {
using json_internal::JSONPathExtractor;
using json_internal::ValidJSONPathIterator;
}  // namespace

JsonPathEvaluator::~JsonPathEvaluator() {}

JsonPathEvaluator::JsonPathEvaluator(std::unique_ptr<ValidJSONPathIterator> itr)
    : path_iterator_(std::move(itr)) {}

// static
zetasql_base::StatusOr<std::unique_ptr<JsonPathEvaluator>> JsonPathEvaluator::Create(
    absl::string_view json_path, bool sql_standard_mode) {
  ZETASQL_ASSIGN_OR_RETURN(std::unique_ptr<ValidJSONPathIterator> itr,
                   ValidJSONPathIterator::Create(json_path, sql_standard_mode));
  // Scan tokens as json_path may not persist beyond this call.
  itr->Scan();
  return absl::WrapUnique(new JsonPathEvaluator(std::move(itr)));
}

absl::Status JsonPathEvaluator::Extract(absl::string_view json,
                                        std::string* value,
                                        bool* is_null) const {
  JSONPathExtractor parser(json, path_iterator_.get());
  parser.set_special_character_escaping(escape_special_characters_);
  parser.set_escaping_needed_callback(&escaping_needed_callback_);
  value->clear();
  parser.Extract(value, is_null);
  if (parser.StoppedDueToStackSpace()) {
    return MakeEvalError() << "JSON parsing failed due to deeply nested "
                              "array/struct. Maximum nesting depth is "
                           << JSONPathExtractor::kMaxParsingDepth;
  }
  return absl::OkStatus();
}

absl::optional<JSONValueConstRef> JsonPathEvaluator::Extract(
    JSONValueConstRef input) const {
  bool first_token = true;
  for (path_iterator_->Rewind(); !path_iterator_->End(); ++(*path_iterator_)) {
    const ValidJSONPathIterator::Token& token = *(*path_iterator_);

    if (first_token) {
      // The JSONPath "$.a[1].b" will result in the following list of tokens:
      // "", "a", "1", "b". The first token is always the empty token
      // corresponding to the whole JSON document, and we don't need to do
      // anything in that iteration. There can be other empty tokens after
      // the first one (empty keys are valid).
      first_token = false;
      continue;
    }

    if (input.IsObject()) {
      absl::optional<JSONValueConstRef> optional_member =
          input.GetMemberIfExists(token);
      if (!optional_member.has_value()) {
        return absl::nullopt;
      }
      input = optional_member.value();
    } else if (input.IsArray()) {
      int64_t index;
      if (!absl::SimpleAtoi(token, &index) || index < 0 ||
          index >= input.GetArraySize()) {
        return absl::nullopt;
      }
      input = input.GetArrayElement(index);
    } else {
      // The path is not present in the JSON object.
      return absl::nullopt;
    }
  }

  return input;
}

absl::Status JsonPathEvaluator::ExtractScalar(absl::string_view json,
                                              std::string* value,
                                              bool* is_null) const {
  json_internal::JSONPathExtractScalar scalar_parser(json,
                                                     path_iterator_.get());
  value->clear();
  scalar_parser.Extract(value, is_null);
  if (scalar_parser.StoppedDueToStackSpace()) {
    return MakeEvalError() << "JSON parsing failed due to deeply nested "
                              "array/struct. Maximum nesting depth is "
                           << JSONPathExtractor::kMaxParsingDepth;
  }
  return absl::OkStatus();
}

absl::optional<std::string> JsonPathEvaluator::ExtractScalar(
    JSONValueConstRef input) const {
  absl::optional<JSONValueConstRef> optional_json = Extract(input);
  if (!optional_json.has_value() || optional_json->IsNull() ||
      optional_json->IsObject() || optional_json->IsArray()) {
    return absl::nullopt;
  }

  if (optional_json->IsString()) {
    // ToString() adds extra quotes and escapes special characters,
    // which we don't want.
    return optional_json->GetString();
  }

  return optional_json->ToString();
}

absl::Status JsonPathEvaluator::ExtractArray(absl::string_view json,
                                             std::vector<std::string>* value,
                                             bool* is_null) const {
  json_internal::JSONPathArrayExtractor array_parser(json,
                                                     path_iterator_.get());
  array_parser.set_special_character_escaping(escape_special_characters_);
  value->clear();
  array_parser.ExtractArray(value, is_null);
  if (array_parser.StoppedDueToStackSpace()) {
    return MakeEvalError() << "JSON parsing failed due to deeply nested "
                              "array/struct. Maximum nesting depth is "
                           << JSONPathExtractor::kMaxParsingDepth;
  }
  return absl::OkStatus();
}

absl::optional<std::vector<JSONValueConstRef>> JsonPathEvaluator::ExtractArray(
    JSONValueConstRef input) const {
  absl::optional<JSONValueConstRef> json = Extract(input);
  if (!json.has_value() || json->IsNull() || !json->IsArray()) {
    return absl::nullopt;
  }

  return json->GetArrayElements();
}

absl::Status JsonPathEvaluator::ExtractStringArray(
    absl::string_view json, std::vector<absl::optional<std::string>>* value,
    bool* is_null) const {
  json_internal::JSONPathStringArrayExtractor array_parser(
      json, path_iterator_.get());
  value->clear();
  array_parser.ExtractStringArray(value, is_null);
  if (array_parser.StoppedDueToStackSpace()) {
    return MakeEvalError() << "JSON parsing failed due to deeply nested "
                              "array/struct. Maximum nesting depth is "
                           << JSONPathExtractor::kMaxParsingDepth;
  }
  return absl::OkStatus();
}

absl::optional<std::vector<absl::optional<std::string>>>
JsonPathEvaluator::ExtractStringArray(JSONValueConstRef input) const {
  absl::optional<std::vector<JSONValueConstRef>> json_array =
      ExtractArray(input);
  if (!json_array.has_value()) {
    return absl::nullopt;
  }

  std::vector<absl::optional<std::string>> results;
  results.reserve(json_array->size());
  for (JSONValueConstRef element : *json_array) {
    if (element.IsArray() || element.IsObject()) {
      return absl::nullopt;
    }

    if (element.IsNull()) {
      results.push_back(absl::nullopt);
    } else if (element.IsString()) {
      // ToString() adds extra quotes and escapes special characters,
      // which we don't want.
      results.push_back(element.GetString());
    } else {
      results.push_back(element.ToString());
    }
  }
  return results;
}

std::string ConvertJSONPathTokenToSqlStandardMode(
    absl::string_view json_path_token) {
  // See json_internal.cc for list of characters that don't need escaping.
  static const RE2& kSpecialCharsPattern =
      *new RE2(R"([^\p{L}\p{N}\d_\-\:\s])");
  static const RE2& kDoubleQuotesPattern = *new RE2(R"(")");

  if (!RE2::PartialMatch(json_path_token, kSpecialCharsPattern)) {
    // No special characters. Can be field access or array element access.
    // Note that '$[0]' is equivalent to '$.0'.
    return std::string(json_path_token);
  } else if (absl::StrContains(json_path_token, "\"")) {
    // We need to escape double quotes in the json_path_token because the SQL
    // standard mode use them to wrap around json_path_token with special
    // characters.
    std::string escaped(json_path_token);
    // Two backslashes are needed in the replacement string because \<digit>
    // is used for group matching.
    RE2::GlobalReplace(&escaped, kDoubleQuotesPattern, R"(\\")");
    return absl::StrCat("\"", escaped, "\"");
  } else {
    // Special characters but no double quotes.
    return absl::StrCat("\"", json_path_token, "\"");
  }
}

zetasql_base::StatusOr<std::string> ConvertJSONPathToSqlStandardMode(
    absl::string_view json_path) {
  // See json_internal.cc for list of characters that don't need escaping.
  static const RE2& kSpecialCharsPattern =
      *new RE2(R"([^\p{L}\p{N}\d_\-\:\s])");
  static const RE2& kDoubleQuotesPattern = *new RE2(R"(")");

  ZETASQL_ASSIGN_OR_RETURN(auto iterator, json_internal::ValidJSONPathIterator::Create(
                                      json_path, /*sql_standard_mode=*/false));

  std::string new_json_path = "$";

  // First token is empty.
  ++(*iterator);

  for (; !iterator->End(); ++(*iterator)) {
    // Token is unescaped.
    absl::string_view token = **iterator;
    if (token.empty()) {
      // Special case: empty token needs to be escaped.
      absl::StrAppend(&new_json_path, ".\"\"");
    } else if (!RE2::PartialMatch(token, kSpecialCharsPattern)) {
      // No special characters. Can be field access or array element access.
      // Note that '$[0]' is equivalent to '$.0'.
      absl::StrAppend(&new_json_path, ".", token);
    } else if (absl::StrContains(token, "\"")) {
      // We need to escape double quotes in the token because the SQL standard
      // mode use them to wrap around token with special characters.
      std::string escaped(token);
      // Two backslashes are needed in the replacement string because \<digit>
      // is used for group matching.
      RE2::GlobalReplace(&escaped, kDoubleQuotesPattern, R"(\\")");
      absl::StrAppend(&new_json_path, ".\"", escaped, "\"");
    } else {
      // Special characters but no double quotes.
      absl::StrAppend(&new_json_path, ".\"", token, "\"");
    }
  }

  ZETASQL_RET_CHECK_OK(json_internal::IsValidJSONPath(new_json_path,
                                              /*sql_standard_mode=*/true));

  return new_json_path;
}

zetasql_base::StatusOr<std::string> MergeJSONPathsIntoSqlStandardMode(
    absl::Span<const std::string> json_paths) {
  if (json_paths.empty()) {
    return absl::OutOfRangeError("Empty JSONPaths.");
  }

  std::string merged_json_path = "$";

  for (absl::string_view json_path : json_paths) {
    if (json_internal::IsValidJSONPath(json_path, /*sql_standard_mode=*/true)
            .ok()) {
      // Remove the "$" prefix.
      absl::StrAppend(&merged_json_path, json_path.substr(1));
    } else {
      // Convert to SQL standard mode first.
      ZETASQL_ASSIGN_OR_RETURN(std::string sql_standard_json_path,
                       ConvertJSONPathToSqlStandardMode(json_path));

      absl::StrAppend(&merged_json_path, sql_standard_json_path.substr(1));
    }
  }

  ZETASQL_RET_CHECK_OK(json_internal::IsValidJSONPath(merged_json_path,
                                              /*sql_standard_mode=*/true));

  return merged_json_path;
}

}  // namespace functions
}  // namespace zetasql

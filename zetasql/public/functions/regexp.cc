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

#include "zetasql/public/functions/regexp.h"

#include <ctype.h>

#include <algorithm>

#include "zetasql/base/logging.h"
#include "zetasql/common/utf_util.h"
#include "zetasql/public/functions/string.h"
#include "zetasql/public/functions/util.h"
#include "absl/memory/memory.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/substitute.h"
#include "absl/types/optional.h"
#include "unicode/utf8.h"
#include "zetasql/base/status.h"
#include "zetasql/base/status_macros.h"

namespace zetasql {
namespace functions {

bool RegExp::InitializePatternUtf8(absl::string_view pattern,
                                   absl::Status* error) {
  RE2::Options options;
  options.set_log_errors(false);
  options.set_encoding(RE2::Options::EncodingUTF8);
  re_ = absl::make_unique<RE2>(pattern, options);
  if (!re_->ok()) {
    return internal::UpdateError(
        error, absl::StrCat("Cannot parse regular expression: ", re_->error()));
  }
  return true;
}

bool RegExp::InitializePatternBytes(absl::string_view pattern,
                                    absl::Status* error) {
  RE2::Options options;
  options.set_log_errors(false);
  options.set_encoding(RE2::Options::EncodingLatin1);
  re_ = absl::make_unique<RE2>(pattern, options);
  if (!re_->ok()) {
    return internal::UpdateError(
        error, absl::StrCat("Cannot parse regular expression: ", re_->error()));
  }
  return true;
}

bool RegExp::Contains(absl::string_view str, bool* out, absl::Status* error) {
  DCHECK(re_);
  *out = re_->PartialMatch(str, *re_);
  return true;
}

bool RegExp::Match(absl::string_view str, bool* out, absl::Status* error) {
  DCHECK(re_);
  *out = re_->FullMatch(str, *re_);
  return true;
}

bool RegExp::Extract(absl::string_view str, PositionUnit position_unit,
                     int64_t position, int64_t occurrence_index,
                     absl::string_view* out, bool* is_null,
                     absl::Status* error) {
  DCHECK(re_);
  *is_null = true;
  *error = internal::ValidatePositionAndOccurrence(position, occurrence_index);
  if (!error->ok()) {
    return false;
  }
  int32_t str_length32 = 0;
  if (!CheckAndCastStrLength(str, &str_length32)) {
    return internal::UpdateError(
        error,
        absl::Substitute("Input string size too large $0", str.length()));
  }
  if (position > str_length32 && !(str.empty() && position == 1)) {
    return true;
  }
  int64_t offset = 0;
  if (position_unit == kUtf8Chars) {
    auto string_offset = ForwardN(str, str_length32, position - 1);
    if (string_offset == absl::nullopt) {
      return true;
    }
    offset = string_offset.value();
  } else {
    offset = position - 1;
  }
  str.remove_prefix(offset);
  if (str.data() == nullptr) {
    // Ensure that the output string never has a null data pointer if a match is
    // found.
    str = absl::string_view("", 0);
  }
  ExtractAllReset(str);

  for (int64_t current_index = 0; current_index < occurrence_index;
       ++current_index) {
    if (!ExtractAllNext(out, error)) {
      return error->ok();
    }
    if (!error->ok()) return false;
  }
  if (out->data() != nullptr) *is_null = false;
  return true;
}

void RegExp::ExtractAllReset(const absl::string_view str) {
  extract_all_input_ = str;
  extract_all_position_ = 0;
  last_match_ = false;
}

bool RegExp::ExtractAllNext(absl::string_view* out, absl::Status* error) {
  DCHECK(re_);
  if (re_->NumberOfCapturingGroups() > 1) {
    return internal::UpdateError(error,
                                 "Regular expressions passed into extraction "
                                 "functions must not have more "
                                 "than 1 capturing group");
  }
  if (last_match_) {
    *out = absl::string_view(nullptr, 0);
    return false;
  }
  absl::string_view groups[2];
  if (!re_->Match(extract_all_input_,
                  extract_all_position_,      // startpos
                  extract_all_input_.size(),  // endpos
                  RE2::UNANCHORED, groups,
                  2)) {  // number of matches at out.
    *out = absl::string_view(nullptr, 0);
    // No matches found in the remaining of the input string.
    return false;
  }

  extract_all_position_ = groups[0].end() - extract_all_input_.begin();
  if (re_->NumberOfCapturingGroups() == 0) {
    // If there's no capturing subgroups return the entire matching substring.
    *out = groups[0];
  } else {
    // If there's a single capturing group return substring matching that
    // group.
    *out = groups[1];
    capture_group_position_ = static_cast<int>(
        groups[1].end() - extract_all_input_.begin());
  }
  // RE2 library produces empty groups[0] when regular expression matches empty
  // string, so in this case we need to advance input by one character.
  if (groups[0].empty() &&
      extract_all_position_ < static_cast<int64_t>(extract_all_input_.size())) {
    if (re_->options().encoding() == RE2::Options::EncodingUTF8) {
      constexpr int64_t kMaxUtf8Length = 4;
      int64_t length_after_position =
          extract_all_input_.size() - extract_all_position_;
      int32_t length_after_position32 =
          static_cast<int32_t>(std::min(kMaxUtf8Length, length_after_position));

      int32_t character_length = 0;
      UChar32 character;
      U8_NEXT(&extract_all_input_[extract_all_position_], character_length,
              length_after_position32, character);
      extract_all_position_ += character_length;

      if (character < 0) {
        error->Update(
            absl::Status(absl::StatusCode::kOutOfRange,
                         "Input argument to REGEXP_EXTRACT_ALL function "
                         "is not valid UTF8 string"));
        return false;
      }
    } else {
      extract_all_position_++;
    }
  }
  // No more input - next call to ExtractAllNext will return false.
  if (extract_all_position_ >= static_cast<int64_t>(extract_all_input_.size())) {
    last_match_ = true;
  }
  return true;
}

bool RegExp::Instr(const InstrParams& options, absl::Status* error) {
  DCHECK(re_ != nullptr);
  DCHECK(error != nullptr);
  DCHECK(options.out != nullptr);
  absl::string_view str = options.input_str;
  *options.out = 0;
  *error = internal::ValidatePositionAndOccurrence(options.position,
                                                   options.occurrence_index);
  if (!error->ok()) {
    return false;  // position or occurrence_index <= 0
  }
  int32_t str_length32 = 0;
  if (!CheckAndCastStrLength(str, &str_length32)) {
    return internal::UpdateError(
        error,
        absl::Substitute("Input string size too large $0", str.length()));
  }
  if (options.position > str_length32 || re_->pattern().empty()) {
    return true;
  }
  int64_t offset = 0;
  if (options.position_unit == kUtf8Chars) {
    auto string_offset = ForwardN(str, str_length32, options.position - 1);
    if (string_offset == absl::nullopt) {
      return true;  // input str is an invalid utf-8 string
    }
    offset = string_offset.value();
  } else {
    offset = options.position - 1;
  }
  str.remove_prefix(offset);
  ExtractAllReset(str);
  absl::string_view next_match;
  for (int64_t current_index = 0; current_index < options.occurrence_index;
    ++current_index) {
    if (!ExtractAllNext(&next_match, error)) {
      return error->ok();
    }
    if (!error->ok()) return false;
  }
  if (next_match.data() == nullptr) {
    return true;
  }
  int32_t visited_bytes = 0;
  if (re_->NumberOfCapturingGroups() == 0) {
    // extract_all_position_ and capture_group_position_ are the indices based
    // on bytes
    visited_bytes = extract_all_position_;
  } else {
    visited_bytes = capture_group_position_;
  }
  if (options.return_position == kStartOfMatch) {
    visited_bytes -= next_match.length();
  }
  if (options.position_unit == kUtf8Chars) {
    // visited_bytes is the length of bytes before the position will be returned
    // We need to convert byte length to character length if the input is a
    // utf-8 string, for example, щ is one character but takes 2 bytes
    absl::string_view prev_str;
    if (!LeftBytes(str, visited_bytes, &prev_str, error)) {
      return false;
    }
    int64_t utf8_size = 0;
    if (!LengthUtf8(prev_str, &utf8_size, error)) {
      return false;
    }
    *options.out = utf8_size + options.position;
  } else {
    *options.out = visited_bytes + options.position;
  }
  return true;
}

bool RegExp::Replace(absl::string_view str, absl::string_view newsub,
                     std::string* out, absl::Status* error) {
  // The following implementation is similar to RE2::GlobalReplace, with a few
  // important differences: (1) it works correctly with UTF-8 strings,
  // (2) it returns proper error message instead of logging it, and
  // (3) limits the size of output string.

  DCHECK(re_);

  std::string error_string;
  if (!re_->CheckRewriteString(newsub, &error_string)) {
    error->Update(absl::Status(
        absl::StatusCode::kOutOfRange,
        absl::StrCat("Invalid REGEXP_REPLACE pattern: ", error_string)));
    return false;
  }

  // "newsub" can reference at most 9 capturing groups indexed 1 to 9. Index 0
  // is reserved for the entire matching substring.
  std::vector<absl::string_view> match(10);

  out->clear();
  // Points to the end of the previous match. This is necessary if the regular
  // expression can match both empty string and some non-empty string, so that
  // we don't replace an empty match immediately following non-empty match.
  // Initialized to str.begin() - 1 instead of nullptr, so that if str is
  // string_view(nullptr, 0) and the pattern of this RegExp is also empty, the
  // condition "match[0].begin() != lastend" below will still be true for once.
  absl::string_view::iterator lastend = str.begin() + std::string_view::npos;
  for (absl::string_view::iterator p = str.begin(); p <= str.end(); ) {
    // Find the first matching substring starting at p and store the
    // match and captured groups in vector 'match'.
    if (!re_->Match(str, p - str.begin(), str.size(),
                    RE2::UNANCHORED, /* match any substring */
                    match.data(), match.size())) {
      out->append(p, str.end());
      break;
    }
    // Emit text until the start of the match verbatim, and then emit
    // the rewritten match.
    out->append(p, match[0].begin());
    p = match[0].begin();
    if (!match[0].empty()) {
      if (!Rewrite(newsub, match, out, error)) return false;
      p = match[0].end();
    } else {
      // The regexp matches empty substring. Ignore the match if it starts at
      // the end of the previous one.
      if (match[0].begin() != lastend &&
          !Rewrite(newsub, match, out, error)) {
        return false;
      }
      if (p < str.end()) {
        // Move p one character forward.
        int32_t len;
        if (re_->options().encoding() == RE2::Options::EncodingUTF8) {
          int32_t char_len = 0;
          constexpr std::ptrdiff_t kMaxUtf8Length = 4;
          U8_FWD_1(p, char_len, std::min(kMaxUtf8Length, str.end() - p));
          len = char_len;
        } else {
          len = 1;
        }
        out->append(p, len);
        p += len;
      } else {
        break;
      }
    }
    lastend = match[0].end();
  }

  return true;
}

void RegExp::SetMaxOutSize(int32_t size) {
  max_out_size_ = size;
}

bool RegExp::Rewrite(const absl::string_view rewrite,
                     const std::vector<absl::string_view>& groups,
                     std::string* out, absl::Status* error) {
  for (const char *s = rewrite.data(); s < rewrite.end(); ++s) {
    const char* start = s;
    while (s < rewrite.end() && *s != '\\') s++;
    out->append(start, s);

    if (s < rewrite.end()) {
      s++;
      int c = (s < rewrite.end()) ? *s : -1;
      if (isdigit(c)) {
        int n = (c - '0');
        out->append(groups[n].data(), groups[n].size());
      } else if (c == '\\') {
        out->push_back('\\');
      } else {
        error->Update(absl::Status(absl::StatusCode::kInternal,
                                   "Invalid REGEXP_REPLACE pattern"));
        return false;
      }
    }

    if (out->length() > max_out_size_) {
      error->Update(absl::Status(absl::StatusCode::kOutOfRange,
                                 "REGEXP_REPLACE: exceeded maximum output "
                                 "length"));
      return false;
    }
  }
  return true;
}

}  // namespace functions
}  // namespace zetasql

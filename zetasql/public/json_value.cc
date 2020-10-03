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

#include "zetasql/public/json_value.h"

#define JSON_NOEXCEPTION
#define JSON_THROW_USER(exception) LOG(FATAL) << (exception).what();

#include <stddef.h>
#include <stdint.h>

#include <memory>
#include <optional>
#include <queue>
#include <string>
#include <utility>
#include <vector>


#include "zetasql/base/logging.h"
#include "zetasql/common/json_parser.h"
#include <cstdint>  
#include "absl/base/optimization.h"
#include "absl/status/status.h"
#include "zetasql/base/statusor.h"
#include "absl/strings/numbers.h"
#include "absl/strings/str_split.h"
#include "absl/strings/string_view.h"
#include "absl/strings/substitute.h"
#include "single_include/nlohmann/json.hpp"
#include "zetasql/base/status_macros.h"

namespace zetasql {

using JSON = ::nlohmann::json;
using ::zetasql_base::StatusOr;

namespace {

// A helper class that is used by the two parser implementations,
// JSONValueLegacyParser and JSONValueStandardParser to construct a JSON
// document tree from a given JSON string.
class JSONValueBuilder {
 public:
  // Constructs a builder that adds content to the given 'value'.
  explicit JSONValueBuilder(JSON& value) : value_(value) {}

  // Resets the builder with a new 'value' to construct.
  void Reset(JSON& value) {
    value_ = value;
    ref_stack_.clear();
    object_member_ = nullptr;
  }

  absl::Status BeginObject() {
    auto result = HandleValue(JSON::value_t::object);
    ZETASQL_ASSIGN_OR_RETURN(ref_stack_.emplace_back(), result);
    return absl::OkStatus();
  }

  absl::Status EndObject() {
    ref_stack_.pop_back();
    return absl::OkStatus();
  }

  absl::Status BeginMember(const std::string& key) {
    // Add null at given key and store the reference for later
    object_member_ = &(ref_stack_.back()->operator[](key));
    return absl::OkStatus();
  }

  absl::Status BeginArray() {
    auto result = HandleValue(JSON::value_t::array);
    ZETASQL_ASSIGN_OR_RETURN(ref_stack_.emplace_back(), result);
    return absl::OkStatus();
  }

  absl::Status EndArray() {
    ref_stack_.pop_back();
    return absl::OkStatus();
  }

  absl::Status ParsedString(const std::string& str) {
    return HandleValue(str).status();
  }

  absl::Status ParsedNumber(absl::string_view str) {
    // To match the nlohmann json library behavior, first try to parse 'str' as
    // unsigned int and only fallback to int if the value is signed integer.
    // This is to make sure that is_number_unsigned() and is_number_integer()
    // both return true for unsigned integers.
    uint64_t uint64_value;
    if (absl::SimpleAtoi(str, &uint64_value)) {
      return HandleValue(uint64_value).status();
    }
    int64_t int64_value;
    if (absl::SimpleAtoi(str, &int64_value)) {
      return HandleValue(int64_value).status();
    }
    double double_value;
    if (absl::SimpleAtod(str, &double_value)) {
      return HandleValue(double_value).status();
    }
    return absl::InternalError(
        absl::Substitute("Attempting to parse invalid JSON number $0", str));
  }

  absl::Status ParsedInt(int64_t val) { return HandleValue(val).status(); }
  absl::Status ParsedUInt(uint64_t val) { return HandleValue(val).status(); }
  absl::Status ParsedDouble(double val) { return HandleValue(val).status(); }
  absl::Status ParsedBool(bool val) { return HandleValue(val).status(); }
  absl::Status ParsedNull() { return HandleValue(nullptr).status(); }

 private:
  // Adds the given value into the currently constructed document tree.
  // Returns non-ok status in case of failure. Otherwise, returns the pointer
  // to the added value.
  template <typename Value>
  zetasql_base::StatusOr<JSON*> HandleValue(Value&& v) {
    if (ref_stack_.empty()) {
      value_ = JSON(std::forward<Value>(v));
      return &value_;
    }

    if (!ref_stack_.back()->is_array() && !ref_stack_.back()->is_object()) {
      return absl::InternalError(
          "Encountered invalid state while parsing JSON.");
    }

    if (ref_stack_.back()->is_array()) {
      ref_stack_.back()->emplace_back(std::forward<Value>(v));
      return &(ref_stack_.back()->back());
    } else {
      CHECK(object_member_);
      *object_member_ = JSON(std::forward<Value>(v));
      return object_member_;
    }
  }

  // The parsed JSON value.
  JSON& value_;
  // Stack to model hierarchy of values.
  std::vector<JSON*> ref_stack_;
  // Helper to hold the reference for the next object element.
  JSON* object_member_ = nullptr;
};

// The base class for JSONValue parsers that provides status tracking.
class JSONValueParserBase {
 public:
  JSONValueParserBase() = default;

  // The current status of the parser.
  absl::Status status() const { return status_; }

 protected:
  // If the given 'status' is not ok, updates the state of the parser to reflect
  // the error only if the parser is not in the error state already. Otherwise
  // does nothing. Returns true if the given 'status' was ok and false
  // otherwise.
  bool MaybeUpdateStatus(absl::Status status) {
    if (ABSL_PREDICT_FALSE(!status.ok())) {
      status_.Update(status);
      return false;
    }
    return true;
  }

 private:
  // Holds any errors encountered by the parser.
  absl::Status status_;
};

// The parser implementation that uses proto based legacy ZetaSQL JSON parser.
class JSONValueLegacyParser : public ::zetasql::JSONParser,
                              public JSONValueParserBase {
 public:
  JSONValueLegacyParser(absl::string_view str, JSON& value)
      : zetasql::JSONParser(str), value_builder_(value) {}

 protected:
  bool BeginObject() override {
    return MaybeUpdateStatus(value_builder_.BeginObject());
  }

  bool EndObject() override {
    return MaybeUpdateStatus(value_builder_.EndObject());
  }

  bool BeginMember(const std::string& key) override {
    return MaybeUpdateStatus(value_builder_.BeginMember(key));
  }

  bool BeginArray() override {
    return MaybeUpdateStatus(value_builder_.BeginArray());
  }

  bool EndArray() override {
    return MaybeUpdateStatus(value_builder_.EndArray());
  }

  bool ParsedString(const std::string& str) override {
    return MaybeUpdateStatus(value_builder_.ParsedString(str));
  }

  bool ParsedNumber(absl::string_view str) override {
    return MaybeUpdateStatus(value_builder_.ParsedNumber(str));
  }

  bool ParsedBool(bool val) override {
    return MaybeUpdateStatus(value_builder_.ParsedBool(val));
  }
  bool ParsedNull() override {
    return MaybeUpdateStatus(value_builder_.ParsedNull());
  }

  bool ReportFailure(const std::string& error_message) override {
    if (status().ok()) {
      MaybeUpdateStatus(absl::InvalidArgumentError(
          absl::Substitute("Parsing JSON string failed: $0", error_message)));
    }
    return false;
  }

 private:
  JSONValueBuilder value_builder_;
};

// The parser implementation that uses nlohmann library implementation based on
// the JSON RFC.
//
// NOTE: Method names are specific requirement of nlohmann SAX parser interface.
class JSONValueStandardParser : public JSONValueParserBase {
 public:
  explicit JSONValueStandardParser(JSON& value) : value_builder_(value) {}
  JSONValueStandardParser() = delete;

  bool null() { return MaybeUpdateStatus(value_builder_.ParsedNull()); }

  bool boolean(bool val) {
    return MaybeUpdateStatus(value_builder_.ParsedBool(val));
  }

  bool number_integer(std::int64_t val) {
    return MaybeUpdateStatus(value_builder_.ParsedInt(val));
  }

  bool number_unsigned(std::uint64_t val) {
    return MaybeUpdateStatus(value_builder_.ParsedUInt(val));
  }

  bool number_float(double val, const std::string& /*unused*/) {
    return MaybeUpdateStatus(value_builder_.ParsedDouble(val));
  }

  bool string(std::string& val) {
    return MaybeUpdateStatus(value_builder_.ParsedString(val));
  }

  bool binary(std::vector<std::uint8_t>& val) {
    // TODO: Implement the binary value type.
    return MaybeUpdateStatus(absl::UnimplementedError(
        "Binary JSON subtypes have not been implemented"));
  }

  bool start_object(std::size_t /*unused*/) {
    return MaybeUpdateStatus(value_builder_.BeginObject());
  }

  bool key(std::string& val) {
    return MaybeUpdateStatus(value_builder_.BeginMember(val));
  }

  bool end_object() { return MaybeUpdateStatus(value_builder_.EndObject()); }

  bool start_array(std::size_t /*unused*/) {
    return MaybeUpdateStatus(value_builder_.BeginArray());
  }

  bool end_array() { return MaybeUpdateStatus(value_builder_.EndArray()); }

  bool parse_error(std::size_t /*unused*/, const std::string& /*unused*/,
                   const nlohmann::detail::exception& ex) {
    std::string error(ex.what());
    // Strip the error code specific to the nlohmann JSON library.
    std::vector<std::string> v = absl::StrSplit(error, "] ");
    if (v.size() > 1) {
      error = v[1];
    }
    return MaybeUpdateStatus(absl::InvalidArgumentError(error));
  }

  bool is_errored() const { return !status().ok(); }

 private:
  JSONValueBuilder value_builder_;
};

}  // namespace

// NOTE: DO NOT CHANGE THIS STRUCT. The JSONValueRef code assumes that
// JSONValue::Impl* can be casted to nlohmann::JSON*.
struct JSONValue::Impl {
  JSON value;
};

StatusOr<JSONValue> JSONValue::ParseJSONString(absl::string_view str,
                                               bool legacy_mode) {
  JSONValue json;
  if (legacy_mode) {
    JSONValueLegacyParser parser(str, json.impl_->value);
    if (!parser.Parse()) {
      if (parser.status().ok()) {
        return absl::InternalError(
            "Parsing JSON failed but didn't return an error");
      } else {
        return parser.status();
      }
    }
  } else {
    JSONValueStandardParser parser(json.impl_->value);
    JSON::sax_parse(str, &parser);
    ZETASQL_RETURN_IF_ERROR(parser.status());
  }

  return json;
}

StatusOr<JSONValue> JSONValue::DeserializeFromProtoBytes(
    absl::string_view str) {
  JSONValue json;
  JSONValueStandardParser parser(json.impl_->value);
  JSON::sax_parse(str, &parser, JSON::input_format_t::ubjson);
  ZETASQL_RETURN_IF_ERROR(parser.status());
  return json;
}

JSONValue JSONValue::CopyFrom(JSONValueConstRef value) {
  JSONValue copy;
  copy.impl_->value = value.impl_->value;
  return copy;
}

JSONValue::JSONValue() : impl_(std::make_unique<Impl>()) {}

JSONValue::JSONValue(int64_t value) : impl_(new Impl{value}) {}
JSONValue::JSONValue(uint64_t value) : impl_(new Impl{value}) {}
JSONValue::JSONValue(double value) : impl_(new Impl{value}) {}
JSONValue::JSONValue(bool value) : impl_(new Impl{value}) {}
JSONValue::JSONValue(std::string value) : impl_(new Impl{std::move(value)}) {}

JSONValue::JSONValue(JSONValue&& value) : impl_(std::move(value.impl_)) {}

JSONValue::~JSONValue() {}

JSONValue& JSONValue::operator=(JSONValue&& value) {
  impl_ = std::move(value.impl_);
  return *this;
}

JSONValueRef JSONValue::GetRef() { return JSONValueRef(impl_.get()); }

JSONValueConstRef JSONValue::GetConstRef() const {
  return JSONValueConstRef(impl_.get());
}

JSONValueConstRef::JSONValueConstRef(const JSONValue::Impl* value_pointer)
    : impl_(value_pointer) {}

bool JSONValueConstRef::IsBoolean() const { return impl_->value.is_boolean(); }

bool JSONValueConstRef::IsNumber() const { return impl_->value.is_number(); }

bool JSONValueConstRef::IsNull() const { return impl_->value.is_null(); }

bool JSONValueConstRef::IsString() const { return impl_->value.is_string(); }

bool JSONValueConstRef::IsObject() const { return impl_->value.is_object(); }

bool JSONValueConstRef::IsArray() const { return impl_->value.is_array(); }

bool JSONValueConstRef::IsInt64() const {
  // is_number_integer() returns true for both signed and unsigned values. We
  // need to make sure that the value fits int64_t if it is unsigned.
  return impl_->value.is_number_integer() &&
         (!impl_->value.is_number_unsigned() || impl_->value.get<int64_t>() >= 0);
}

bool JSONValueConstRef::IsUInt64() const {
  return impl_->value.is_number_unsigned();
}

bool JSONValueConstRef::IsDouble() const {
  return impl_->value.is_number_float();
}

int64_t JSONValueConstRef::GetInt64() const { return impl_->value.get<int64_t>(); }

uint64_t JSONValueConstRef::GetUInt64() const {
  return impl_->value.get<uint64_t>();
}

double JSONValueConstRef::GetDouble() const {
  return impl_->value.get<double>();
}

std::string JSONValueConstRef::GetString() const {
  return impl_->value.get<std::string>();
}

bool JSONValueConstRef::GetBoolean() const { return impl_->value.get<bool>(); }

bool JSONValueConstRef::HasMember(absl::string_view key) const {
  return impl_->value.find(key) != impl_->value.end();
}

JSONValueConstRef JSONValueConstRef::GetMember(absl::string_view key) const {
  return JSONValueConstRef(reinterpret_cast<const JSONValue::Impl*>(
      &impl_->value[std::string(key)]));
}

absl::optional<JSONValueConstRef> JSONValueConstRef::GetMemberIfExists(
    absl::string_view key) const {
  auto iter = impl_->value.find(key);
  if (iter == impl_->value.end()) {
    return absl::nullopt;
  }
  return JSONValueConstRef(
      reinterpret_cast<const JSONValue::Impl*>(&iter.value()));
}

std::vector<std::pair<absl::string_view, JSONValueConstRef>>
JSONValueConstRef::GetMembers() const {
  std::vector<std::pair<absl::string_view, JSONValueConstRef>> members;
  for (auto& member : impl_->value.items()) {
    members.push_back(
        {member.key(),
         JSONValueConstRef(
             reinterpret_cast<const JSONValue::Impl*>(&member.value()))});
  }
  return members;
}

size_t JSONValueConstRef::GetArraySize() const { return impl_->value.size(); }

JSONValueConstRef JSONValueConstRef::GetArrayElement(size_t index) const {
  return JSONValueConstRef(
      reinterpret_cast<const JSONValue::Impl*>(&impl_->value[index]));
}

std::vector<JSONValueConstRef> JSONValueConstRef::GetArrayElements() const {
  std::vector<JSONValueConstRef> elements;
  for (auto& element : impl_->value) {
    elements.emplace_back(
        JSONValueConstRef(reinterpret_cast<const JSONValue::Impl*>(&element)));
  }
  return elements;
}

std::string JSONValueConstRef::ToString() const { return impl_->value.dump(); }

std::string JSONValueConstRef::Format() const {
  return impl_->value.dump(/*indent=*/2);
}

void JSONValueConstRef::SerializeAndAppendToProtoBytes(
    std::string* output) const {
  JSON::to_ubjson(impl_->value, *output);
}

namespace {

uint64_t EstimateStringSpaceUsed(const std::string& str) {
  size_t size = str.capacity() + 1;
  // Small strings are allocated inline in typical string implementations.
  return size < sizeof(JSON::string_t) ? sizeof(JSON::string_t)
                                       : size + sizeof(JSON::string_t);
}

}  // namespace

uint64_t JSONValueConstRef::SpaceUsed() const {
  uint64_t space_used = sizeof(JSONValue);
  std::queue<const JSON*> nodes;
  nodes.push(&impl_->value);
  while (!nodes.empty()) {
    const JSON* node = nodes.front();
    nodes.pop();
    // All values except for objects, arrays, and strings are stored inline in
    // JSON::JSON_value. For objects, arrays, and string, the calculation
    // accounts for the data structure size as well.
    space_used += sizeof(JSON);
    if (node->is_object()) {
      space_used += sizeof(JSON::object_t);
      for (auto& el : node->items()) {
        space_used += EstimateStringSpaceUsed(el.key());
        // Estimate per-element memory usage of std::map using 4 pointers.
        space_used += 4 * sizeof(void*);
        nodes.push(&el.value());
      }
    } else if (node->is_array()) {
      space_used += sizeof(JSON::array_t);
      for (const JSON& element : *node) {
        nodes.push(&element);
      }
    } else if (node->is_string()) {
      space_used += EstimateStringSpaceUsed(node->get<std::string>());
    }
  }
  return space_used;
}

// This equality operation uses nlohmann's implementation.
//
// In this implementation, integers and floating points can be equal by
// casting the integer into a floating point and comparing the numbers as
// floating points. Signed and unsigned integers can also be equal.
bool JSONValueConstRef::NormalizedEquals(JSONValueConstRef that) const {
  return impl_->value == that.impl_->value;
}

JSONValueRef::JSONValueRef(JSONValue::Impl* impl)
    : JSONValueConstRef(impl), impl_(impl) {}

JSONValueRef JSONValueRef::GetMember(absl::string_view key) {
  return JSONValueRef(
      reinterpret_cast<JSONValue::Impl*>(&impl_->value[std::string(key)]));
}

std::vector<std::pair<absl::string_view, JSONValueRef>>
JSONValueRef::GetMembers() {
  std::vector<std::pair<absl::string_view, JSONValueRef>> members;
  for (auto& member : impl_->value.items()) {
    members.push_back(
        {member.key(),
         JSONValueRef(reinterpret_cast<JSONValue::Impl*>(&member.value()))});
  }
  return members;
}

JSONValueRef JSONValueRef::GetArrayElement(size_t index) {
  return JSONValueRef(reinterpret_cast<JSONValue::Impl*>(&impl_->value[index]));
}

std::vector<JSONValueRef> JSONValueRef::GetArrayElements() {
  std::vector<JSONValueRef> elements;
  for (auto& element : impl_->value) {
    elements.emplace_back(
        JSONValueRef(reinterpret_cast<JSONValue::Impl*>(&element)));
  }
  return elements;
}

void JSONValueRef::SetInt64(int64_t value) { impl_->value = value; }

void JSONValueRef::SetUInt64(uint64_t value) { impl_->value = value; }

void JSONValueRef::SetDouble(double value) { impl_->value = value; }

void JSONValueRef::SetString(absl::string_view value) { impl_->value = value; }

void JSONValueRef::SetBoolean(bool value) { impl_->value = value; }

}  // namespace zetasql

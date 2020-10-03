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

#ifndef ZETASQL_PUBLIC_TYPES_SIMPLE_TYPE_H_
#define ZETASQL_PUBLIC_TYPES_SIMPLE_TYPE_H_

#include "zetasql/public/civil_time.h"
#include "zetasql/public/types/type.h"

namespace zetasql {

// SimpleType includes all the non-parameterized builtin types (all scalar types
// except enum).
class SimpleType : public Type {
 public:
  SimpleType(const TypeFactory* factory, TypeKind kind);
#ifndef SWIG
  SimpleType(const SimpleType&) = delete;
  SimpleType& operator=(const SimpleType&) = delete;
#endif  // SWIG

  std::string TypeName(ProductMode mode) const override;

  bool IsSupportedType(const LanguageOptions& language_options) const override;

  // Check whether type with a given name exists and is simple. If yes, returns
  // the type kind of the found simple type. Returns TYPE_UNKNOWN otherwise.
  // If 'language_features' is null, then assume all language features are
  // enabled.
  static TypeKind GetTypeKindIfSimple(
      const absl::string_view type_name, ProductMode mode,
      const std::set<LanguageFeature>* language_features = nullptr);

 protected:
  ~SimpleType() override;

  int64_t GetEstimatedOwnedMemoryBytesSize() const override {
    return sizeof(*this);
  }

 private:
  bool SupportsGroupingImpl(const LanguageOptions& language_options,
                            const Type** no_grouping_type) const override;

  absl::Status SerializeToProtoAndDistinctFileDescriptorsImpl(
      const BuildFileDescriptorMapOptions& options, TypeProto* type_proto,
      FileDescriptorSetMap* file_descriptor_set_map) const override;

  bool EqualsForSameKind(const Type* that, bool equivalent) const override {
    return true;
  }

  void CopyValueContent(const ValueContent& from,
                        ValueContent* to) const override;
  void ClearValueContent(const ValueContent& value) const override;
  uint64_t GetValueContentExternallyAllocatedByteSize(
      const ValueContent& value) const override;
  absl::HashState HashTypeParameter(absl::HashState state) const override;
  absl::HashState HashValueContent(const ValueContent& value,
                                   absl::HashState state) const override;
  bool ValueContentEquals(
      const ValueContent& x, const ValueContent& y,
      const ValueEqualityCheckOptions& options) const override;
  bool ValueContentLess(const ValueContent& x, const ValueContent& y,
                        const Type* other_type) const override;
  std::string FormatValueContent(
      const ValueContent& value,
      const FormatValueContentOptions& options) const override;
  absl::Status SerializeValueContent(const ValueContent& value,
                                     ValueProto* value_proto) const override;
  absl::Status DeserializeValueContent(const ValueProto& value_proto,
                                       ValueContent* value) const override;

  void DebugStringImpl(bool details, TypeOrStringVector* stack,
                       std::string* debug_string) const override;

  // Used for TYPE_TIMESTAMP.
  static absl::Time GetTimestampValue(const ValueContent& value);
  static absl::Status SetTimestampValue(absl::Time time, ValueContent* value);

  // Used for TYPE_TIME.
  static TimeValue GetTimeValue(const ValueContent& value);
  static absl::Status SetTimeValue(TimeValue time, ValueContent* value);

  // Used for TYPE_DATETIME.
  static DatetimeValue GetDateTimeValue(const ValueContent& value);
  static absl::Status SetDateTimeValue(DatetimeValue datetime,
                                       ValueContent* value);

  friend class TypeFactory;
};

}  // namespace zetasql

#endif  // ZETASQL_PUBLIC_TYPES_SIMPLE_TYPE_H_

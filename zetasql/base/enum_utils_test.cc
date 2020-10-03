//
// Copyright 2018 Google LLC
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

#include "zetasql/base/enum_utils.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "zetasql/base/test_payload.pb.h"

namespace zetasql_base {

using ::testing::ElementsAre;

TEST(EnumUtilsTest, EnumerateProtoEnum) {
  auto view = EnumerateEnumValues<EnumEnumerationMessage::Enum>();
  std::vector<EnumEnumerationMessage::Enum> all(view.begin(), view.end());
  EXPECT_THAT(all, ::testing::ElementsAre(
                       EnumEnumerationMessage::ENUM_UNSPECIFIED,
                       EnumEnumerationMessage::A, EnumEnumerationMessage::B,
                       EnumEnumerationMessage::D));
  all.clear();
  for (EnumEnumerationMessage::Enum e :
       EnumerateEnumValues<EnumEnumerationMessage::Enum>()) {
    all.push_back(e);
  }
  EXPECT_THAT(all, ::testing::ElementsAre(
                       EnumEnumerationMessage::ENUM_UNSPECIFIED,
                       EnumEnumerationMessage::A, EnumEnumerationMessage::B,
                       EnumEnumerationMessage::D));
  all.clear();
}

}  // namespace zetasql_base

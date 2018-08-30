// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "garnet/lib/cmx/cmx.h"

#include <fcntl.h>
#include <string>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "lib/fxl/files/path.h"
#include "lib/fxl/files/scoped_temp_dir.h"
#include "lib/fxl/strings/substitute.h"
#include "third_party/rapidjson/rapidjson/document.h"

namespace component {
namespace {

class CmxMetadataTest : public ::testing::Test {
 protected:
  // ExpectFailedParse() will replace '$0' with the JSON filename, if present.
  void ExpectFailedParse(const std::string& json, std::string expected_error) {
    std::string error;
    CmxMetadata cmx;
    std::string json_basename;
    EXPECT_FALSE(ParseFrom(&cmx, json, &json_basename));
    EXPECT_TRUE(cmx.HasError());
    EXPECT_EQ(cmx.error_str(), fxl::Substitute(expected_error, json_basename));
  }

  bool ParseFrom(CmxMetadata* cmx, const std::string& json,
                 std::string* json_basename) {
    std::string json_path;
    if (!tmp_dir_.NewTempFileWithData(json, &json_path)) {
      return false;
    }
    *json_basename = files::GetBaseName(json_path);
    const int dirfd = open(tmp_dir_.path().c_str(), O_RDONLY);
    return cmx->ParseFromFileAt(dirfd, *json_basename);
  }

  bool ParseFromDeprecatedRuntime(CmxMetadata* cmx, const std::string& json,
                                  std::string* json_basename) {
    std::string json_path;
    if (!tmp_dir_.NewTempFileWithData(json, &json_path)) {
      return false;
    }
    *json_basename = files::GetBaseName(json_path);
    const int dirfd = open(tmp_dir_.path().c_str(), O_RDONLY);
    return cmx->ParseFromDeprecatedRuntimeFileAt(dirfd, *json_basename);
  }

 private:
  files::ScopedTempDir tmp_dir_;
};

TEST_F(CmxMetadataTest, ParseMetadata) {
  CmxMetadata cmx;
  const std::string json = R"JSON({
  "sandbox": {
      "dev": [ "class/input" ],
      "features": [ "feature_a" ],
      "services": []
  },
  "runner": "dart_runner",
  "facets": {
    "some_key": "some_value"
  },
  "other": "stuff"
  })JSON";
  std::string file_unused;
  EXPECT_TRUE(ParseFrom(&cmx, json, &file_unused)) << cmx.error_str();
  EXPECT_FALSE(cmx.HasError());

  const auto& sandbox = cmx.sandbox_meta();
  EXPECT_FALSE(sandbox.IsNull());
  EXPECT_THAT(sandbox.dev(), ::testing::ElementsAre("class/input"));
  EXPECT_TRUE(sandbox.HasFeature("feature_a"));
  EXPECT_FALSE(sandbox.HasFeature("feature_b"));

  EXPECT_FALSE(cmx.runtime_meta().IsNull());
  EXPECT_EQ(cmx.runtime_meta().runner(), "dart_runner");

  const auto& facets = cmx.facets_meta();
  const auto& some_value = facets.GetSection("some_key");
  ASSERT_TRUE(some_value.IsString());
  EXPECT_EQ("some_value", std::string(some_value.GetString()));
  const auto& null_value = facets.GetSection("invalid");
  EXPECT_TRUE(null_value.IsNull());
}

TEST_F(CmxMetadataTest, ParseEmpty) {
  // No 'program' or 'runner'. Valid syntax, but empty.
  rapidjson::Value sandbox;
  std::string error;
  const std::string json = R"JSON(
  {
    "sandwich": { "ingredients": [ "bacon", "lettuce", "tomato" ] },
    "sandbox": { "services": [] }
  }
  )JSON";
  CmxMetadata cmx;
  std::string file_unused;
  EXPECT_TRUE(ParseFrom(&cmx, json, &file_unused)) << cmx.error_str();
  EXPECT_FALSE(cmx.sandbox_meta().IsNull());
  EXPECT_TRUE(cmx.runtime_meta().IsNull());
  EXPECT_TRUE(cmx.program_meta().IsNull());
}

TEST_F(CmxMetadataTest, ParseFromDeprecatedRuntime) {
  rapidjson::Value sandbox;
  std::string error;
  const std::string json = R"JSON(
  { "runner": "dart_runner" }
  )JSON";
  CmxMetadata cmx;
  std::string file_unused;
  EXPECT_TRUE(ParseFromDeprecatedRuntime(&cmx, json, &file_unused))
      << cmx.error_str();
  EXPECT_TRUE(cmx.sandbox_meta().IsNull());
  EXPECT_FALSE(cmx.runtime_meta().IsNull());
  EXPECT_EQ("dart_runner", cmx.runtime_meta().runner());
  EXPECT_TRUE(cmx.program_meta().IsNull());
}

#define NO_SERVICES \
    "$0: Sandbox must include either 'services' or " \
    "'deprecated-all-services'.\n" \
    "Refer to " \
    "https://fuchsia.googlesource.com/docs/+/master/the-book/" \
    "package_metadata.md#sandbox for more information."

TEST_F(CmxMetadataTest, ParseWithErrors) {
  rapidjson::Value sandbox;
  std::string error;
  ExpectFailedParse(R"JSON({ ,,, })JSON",
                    "$0:1:3: Missing a name for object member.");
  ExpectFailedParse(R"JSON(3)JSON", "$0: File is not a JSON object.");
  ExpectFailedParse(R"JSON({ "sandbox" : 3})JSON",
                    "$0: 'sandbox' is not an object.");
  ExpectFailedParse(R"JSON({ "sandbox" : {"dev": "notarray"} })JSON",
                    "$0: 'dev' in sandbox is not an array.\n" NO_SERVICES);
  ExpectFailedParse(R"JSON({ "runner" : 3 })JSON",
                    NO_SERVICES "\n$0: 'runner' is not a string.");
  ExpectFailedParse(R"JSON({ "program" : { "binary": 3 } })JSON",
                    NO_SERVICES "\n$0: 'binary' in program is not a string.");
}

TEST_F(CmxMetadataTest, GetDefaultComponentCmxPath) {
  EXPECT_EQ("meta/sysmgr.cmx", CmxMetadata::GetDefaultComponentCmxPath(
                                   "file:///pkgfs/packages/sysmgr/0"));
  EXPECT_EQ(
      "", CmxMetadata::GetDefaultComponentCmxPath("/pkgfs/packages/sysmgr/0"));
  EXPECT_EQ("", CmxMetadata::GetDefaultComponentCmxPath(
                    "file:///pkgfs/nothing/sysmgr/0"));
  EXPECT_EQ("", CmxMetadata::GetDefaultComponentCmxPath(""));
}

}  // namespace
}  // namespace component

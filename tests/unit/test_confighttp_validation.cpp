/**
 * @file tests/unit/test_confighttp_validation.cpp
 * @brief Test src/confighttp_validation.cpp
 */
// lib includes
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

// standard includes
#include <array>
#include <string>

// local includes
#include <src/confighttp_validation.h>

TEST(ConfighttpValidationTest, ValidateClientUuidAcceptsCanonicalValue) {
  ASSERT_TRUE(confighttp_validation::validate_client_uuid("01234567-89ab-cdef-0123-456789abcdef"));
}

TEST(ConfighttpValidationTest, ValidateClientUuidRejectsInvalidValue) {
  ASSERT_FALSE(confighttp_validation::validate_client_uuid(""));
  ASSERT_FALSE(confighttp_validation::validate_client_uuid("not-a-uuid"));
}

TEST(ConfighttpValidationTest, ValidateConfigPatchRejectsUnknownKey) {
  const nlohmann::json patch = {
    {"unknown_key", "value"},
  };

  ASSERT_TRUE(confighttp_validation::validate_config_patch(patch).has_value());
}

TEST(ConfighttpValidationTest, ValidateConfigPatchRejectsNewlineInjection) {
  const nlohmann::json patch = {
    {"sunshine_name", "bad\nname"},
  };

  ASSERT_TRUE(confighttp_validation::validate_config_patch(patch).has_value());
}

TEST(ConfighttpValidationTest, ValidateConfigPatchAcceptsKnownKey) {
  const nlohmann::json patch = {
    {"sunshine_name", "Sunshine Host"},
  };

  ASSERT_FALSE(confighttp_validation::validate_config_patch(patch).has_value());
}

TEST(ConfighttpValidationTest, ValidateAppJsonRejectsUnknownField) {
  const nlohmann::json app = {
    {"name", "Test"},
    {"index", 0},
    {"unexpected", "value"},
  };

  ASSERT_TRUE(confighttp_validation::validate_app_json(app).has_value());
}

TEST(ConfighttpValidationTest, ValidateAppJsonAcceptsMinimalApp) {
  const nlohmann::json app = {
    {"name", "Desktop"},
    {"index", -1},
  };

  ASSERT_FALSE(confighttp_validation::validate_app_json(app).has_value());
}

TEST(ConfighttpValidationTest, ValidatePasswordChangeRejectsShortPassword) {
  const nlohmann::json input = {
    {"newUsername", "admin"},
    {"newPassword", "abc"},
    {"confirmNewPassword", "abc"},
  };

  ASSERT_TRUE(confighttp_validation::validate_password_change(input, true).has_value());
}

TEST(ConfighttpValidationTest, ValidateCoverUploadRejectsLargePayload) {
  ASSERT_TRUE(confighttp_validation::validate_cover_upload("cover", confighttp_validation::MAX_COVER_UPLOAD_BYTES + 1).has_value());
}

TEST(ConfighttpValidationTest, IsValidPngBytesDetectsSignature) {
  static constexpr std::array<unsigned char, 8> png_signature {
    0x89,
    0x50,
    0x4E,
    0x47,
    0x0D,
    0x0A,
    0x1A,
    0x0A,
  };

  ASSERT_TRUE(confighttp_validation::is_valid_png_bytes(std::string(reinterpret_cast<const char *>(png_signature.data()), png_signature.size())));
  ASSERT_FALSE(confighttp_validation::is_valid_png_bytes("not-a-png"));
}

TEST(ConfighttpValidationTest, IsBrowsePathAllowedBlocksEtc) {
#ifndef _WIN32
  ASSERT_FALSE(confighttp_validation::is_browse_path_allowed("/etc"));
  ASSERT_TRUE(confighttp_validation::is_browse_path_allowed("/usr/bin"));
#endif
}

TEST(ConfighttpValidationTest, AuthRateLimitBlocksAfterRepeatedFailures) {
  const std::string address = "127.0.0.2";

  confighttp_validation::auth_rate_limit_clear(address);
  for (std::size_t i = 0; i < 5; ++i) {
    confighttp_validation::auth_rate_limit_record_failure(address);
  }

  ASSERT_TRUE(confighttp_validation::auth_rate_limit_is_blocked(address));
  confighttp_validation::auth_rate_limit_clear(address);
  ASSERT_FALSE(confighttp_validation::auth_rate_limit_is_blocked(address));
}

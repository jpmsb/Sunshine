/**
 * @file src/confighttp_validation.h
 * @brief Input validation and security helpers for the Web UI HTTP server.
 */
#pragma once

// standard includes
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

// lib includes
#include <nlohmann/json.hpp>

namespace confighttp_validation {
  /**
   * @brief Maximum decoded cover upload size in bytes.
   */
  constexpr std::size_t MAX_COVER_UPLOAD_BYTES = 5 * 1024 * 1024;

  /**
   * @brief Return whether authentication attempts from an address are temporarily blocked.
   *
   * @param client_address Normalized remote address.
   * @return True when further authentication attempts should be rejected.
   */
  bool auth_rate_limit_is_blocked(const std::string &client_address);

  /**
   * @brief Record a failed authentication attempt for an address.
   *
   * @param client_address Normalized remote address.
   */
  void auth_rate_limit_record_failure(const std::string &client_address);

  /**
   * @brief Clear recorded authentication failures for an address after success.
   *
   * @param client_address Normalized remote address.
   */
  void auth_rate_limit_clear(const std::string &client_address);

  /**
   * @brief Validate an application JSON object for saveApp.
   *
   * @param app Application payload including optional index field.
   * @return Empty optional on success, otherwise an error message.
   */
  std::optional<std::string> validate_app_json(const nlohmann::json &app);

  /**
   * @brief Validate a configuration patch JSON object for saveConfig.
   *
   * @param patch Configuration key/value patch from the Web UI.
   * @return Empty optional on success, otherwise an error message.
   */
  std::optional<std::string> validate_config_patch(const nlohmann::json &patch);

  /**
   * @brief Validate password change payload for savePassword.
   *
   * @param input Parsed JSON request body.
   * @param initial_setup True when no credentials exist yet.
   * @return Empty optional on success, otherwise an error message.
   */
  std::optional<std::string> validate_password_change(const nlohmann::json &input, bool initial_setup);

  /**
   * @brief Validate a client UUID string.
   *
   * @param uuid Candidate UUID from an API request.
   * @return True when the UUID uses the canonical lowercase hex format.
   */
  bool validate_client_uuid(std::string_view uuid);

  /**
   * @brief Validate a cover upload key and payload size.
   *
   * @param key Cover key from the request.
   * @param data_size Decoded upload size in bytes.
   * @return Empty optional on success, otherwise an error message.
   */
  std::optional<std::string> validate_cover_upload(std::string_view key, std::size_t data_size);

  /**
   * @brief Return whether decoded bytes begin with a PNG signature.
   *
   * @param data Raw file bytes.
   * @return True when the payload looks like a PNG file.
   */
  bool is_valid_png_bytes(std::string_view data);

  /**
   * @brief Return whether a directory path may be listed by browseDirectory.
   *
   * @param dir_path Canonical directory path to inspect.
   * @return True when browsing the path is allowed.
   */
  bool is_browse_path_allowed(const std::filesystem::path &dir_path);

}  // namespace confighttp_validation

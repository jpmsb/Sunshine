/**
 * @file src/confighttp_validation.cpp
 * @brief Definitions for Web UI HTTP validation helpers.
 */
// standard includes
#include <array>
#include <chrono>
#include <cctype>
#include <map>
#include <mutex>
#include <unordered_set>

// lib includes
#include <boost/algorithm/string.hpp>
#include <lizardbyte/common/env.h>

// local includes
#include "confighttp_validation.h"
#include "config.h"
#include "platform/common.h"
#include "process.h"

namespace confighttp_validation {
  namespace fs = std::filesystem;

  using namespace std::literals;

  constexpr std::size_t MAX_APP_STRING_LENGTH = 4096;  ///< Max length for generic application string fields.
  constexpr std::size_t MAX_APP_NAME_LENGTH = 256;  ///< Max length for an application display name.
  constexpr std::size_t MAX_USERNAME_LENGTH = 64;  ///< Max length for Web UI usernames.
  constexpr std::size_t MIN_PASSWORD_LENGTH = 4;  ///< Minimum Web UI password length.
  constexpr std::size_t MAX_PASSWORD_LENGTH = 128;  ///< Maximum Web UI password length.
  constexpr std::size_t MAX_COVER_KEY_LENGTH = 128;  ///< Max length for cover upload keys.

  constexpr std::size_t MAX_AUTH_FAILURES_PER_ADDRESS = 5;  ///< Failed logins before temporary lockout.
  constexpr auto AUTH_LOCKOUT_DURATION = std::chrono::minutes(15);  ///< Duration of an auth lockout window.

  /**
   * @brief Track failed authentication attempts per remote address.
   */
  struct auth_failure_state_t {
    std::size_t failures = 0;  ///< Consecutive failed attempts.
    std::chrono::steady_clock::time_point locked_until {};  ///< Lockout expiry when blocked.
  };

  std::map<std::string, auth_failure_state_t, std::less<>> auth_failures;  ///< Auth failures by address.
  std::mutex auth_failures_mutex;  ///< Mutex protecting auth failure state.

  /**
   * @brief Return whether a string contains disallowed control characters.
   *
   * @param value Candidate string value.
   * @return True when newline, NUL, or other control characters are present.
   */
  bool contains_disallowed_control_chars(std::string_view value) {
    return std::ranges::any_of(value, [](unsigned char ch) {
      return ch == '\0' || ch == '\n' || ch == '\r' || ch < 0x20;
    });
  }

  /**
   * @brief Return whether a canonical path is equal to or nested under a root path.
   *
   * @param path Canonical path to test.
   * @param root Canonical root path.
   * @return True when path is root or a descendant of root.
   */
  bool path_is_under_root(const fs::path &path, const fs::path &root) {
    if (root.empty()) {
      return false;
    }

    std::error_code ec;
    const auto rel = fs::relative(path, root, ec);
    if (ec || rel.empty()) {
      return path == root;
    }

    return rel.begin()->string() != "..";
  }

  /**
   * @brief Return whether a canonical path starts with a blocked prefix.
   *
   * @param path Canonical path to test.
   * @param prefix Blocked prefix path.
   * @return True when path is equal to or nested under prefix.
   */
  bool path_has_blocked_prefix(const fs::path &path, const fs::path &prefix) {
    if (prefix.empty()) {
      return false;
    }

    return path_is_under_root(path, prefix);
  }

  /**
   * @brief Validate a bounded string field.
   *
   * @param field_name Field label used in error messages.
   * @param value Candidate value.
   * @param max_length Maximum allowed length.
   * @param allow_empty Whether an empty value is permitted.
   * @return Empty optional on success, otherwise an error message.
   */
  std::optional<std::string> validate_string_field(std::string_view field_name, std::string_view value, std::size_t max_length, bool allow_empty) {
    if (!allow_empty && value.empty()) {
      return std::format("Invalid {}", field_name);
    }

    if (value.size() > max_length) {
      return std::format("{} is too long", field_name);
    }

    if (contains_disallowed_control_chars(value)) {
      return std::format("{} contains invalid characters", field_name);
    }

    return std::nullopt;
  }

  /**
   * @brief Validate a prep-cmd array entry.
   *
   * @param entry Candidate prep command object.
   * @return Empty optional on success, otherwise an error message.
   */
  std::optional<std::string> validate_prep_cmd_entry(const nlohmann::json &entry) {
    if (!entry.is_object()) {
      return "Invalid prep-cmd entry";
    }

    static constexpr std::array<std::string_view, 3> allowed_keys {"do"sv, "undo"sv, "elevated"sv};
    for (const auto &[key, _] : entry.items()) {
      if (std::ranges::find(allowed_keys, key) == allowed_keys.end()) {
        return "Invalid prep-cmd entry";
      }
    }

    if (entry.contains("do")) {
      if (!entry.at("do").is_string()) {
        return "Invalid prep-cmd do field";
      }
      if (const auto err = validate_string_field("prep-cmd do", entry.at("do").get<std::string>(), MAX_APP_STRING_LENGTH, true)) {
        return err;
      }
    }

    if (entry.contains("undo")) {
      if (!entry.at("undo").is_string()) {
        return "Invalid prep-cmd undo field";
      }
      if (const auto err = validate_string_field("prep-cmd undo", entry.at("undo").get<std::string>(), MAX_APP_STRING_LENGTH, true)) {
        return err;
      }
    }

    if (entry.contains("elevated") && !entry.at("elevated").is_boolean()) {
      return "Invalid prep-cmd elevated field";
    }

    return std::nullopt;
  }

  /**
   * @brief Return the static allowlist of Web UI configuration keys.
   *
   * @return Allowed configuration keys for saveConfig.
   */
  const std::unordered_set<std::string> &config_key_allowlist() {
    static const std::unordered_set<std::string> keys {
      "locale",
      "sunshine_name",
      "min_log_level",
      "global_prep_cmd",
      "notify_pre_releases",
      "system_tray",
      "controller",
      "gamepad",
      "ds4_back_as_touchpad_click",
      "motion_as_ds4",
      "touchpad_as_ds4",
      "ds5_inputtino_randomize_mac",
      "back_button_timeout",
      "keyboard",
      "key_repeat_delay",
      "key_repeat_frequency",
      "always_send_scancodes",
      "key_rightalt_to_key_win",
      "mouse",
      "high_resolution_scrolling",
      "native_pen_touch",
      "keybindings",
      "audio_sink",
      "virtual_sink",
      "stream_audio",
      "install_steam_audio_drivers",
      "adapter_name",
      "output_name",
      "dd_configuration_option",
      "dd_resolution_option",
      "dd_manual_resolution",
      "dd_refresh_rate_option",
      "dd_manual_refresh_rate",
      "dd_hdr_option",
      "dd_wa_hdr_toggle_delay",
      "dd_config_revert_delay",
      "dd_config_revert_on_disconnect",
      "dd_mode_remapping",
      "max_bitrate",
      "minimum_fps_target",
      "upnp",
      "address_family",
      "bind_address",
      "web_ui_bind_address",
      "port",
      "origin_web_ui_allowed",
      "csrf_allowed_origins",
      "external_ip",
      "lan_encryption_mode",
      "wan_encryption_mode",
      "ping_timeout",
      "packetsize",
      "file_apps",
      "credentials_file",
      "log_path",
      "pkey",
      "cert",
      "file_state",
      "fec_percentage",
      "qp",
      "min_threads",
      "hevc_mode",
      "av1_mode",
      "capture",
      "encoder",
      "nvenc_preset",
      "nvenc_twopass",
      "nvenc_spatial_aq",
      "nvenc_vbv_increase",
      "nvenc_realtime_hags",
      "nvenc_split_encode",
      "nvenc_latency_over_power",
      "nvenc_opengl_vulkan_on_dxgi",
      "nvenc_h264_cavlc",
      "qsv_preset",
      "qsv_coder",
      "qsv_slow_hevc",
      "amd_usage",
      "amd_rc",
      "amd_enforce_hrd",
      "amd_quality",
      "amd_preanalysis",
      "amd_vbaq",
      "amd_coder",
      "vt_coder",
      "vt_software",
      "vt_realtime",
      "vaapi_strict_rc_buffer",
      "vk_tune",
      "vk_rc_mode",
      "sw_preset",
      "sw_tune",
    };

    return keys;
  }

  bool auth_rate_limit_is_blocked(const std::string &client_address) {
    if (client_address.empty()) {
      return false;
    }

    const auto now = std::chrono::steady_clock::now();
    std::scoped_lock lock(auth_failures_mutex);

    const auto it = auth_failures.find(client_address);
    if (it == auth_failures.end()) {
      return false;
    }

    if (it->second.locked_until != std::chrono::steady_clock::time_point {} && it->second.locked_until > now) {
      return true;
    }

    if (it->second.locked_until != std::chrono::steady_clock::time_point {} && it->second.locked_until <= now) {
      auth_failures.erase(it);
    }

    return false;
  }

  void auth_rate_limit_record_failure(const std::string &client_address) {
    if (client_address.empty()) {
      return;
    }

    const auto now = std::chrono::steady_clock::now();
    std::scoped_lock lock(auth_failures_mutex);

    auto &state = auth_failures[client_address];
    ++state.failures;

    if (state.failures >= MAX_AUTH_FAILURES_PER_ADDRESS) {
      state.locked_until = now + AUTH_LOCKOUT_DURATION;
      state.failures = 0;
    }
  }

  void auth_rate_limit_clear(const std::string &client_address) {
    if (client_address.empty()) {
      return;
    }

    std::scoped_lock lock(auth_failures_mutex);
    auth_failures.erase(client_address);
  }

  std::optional<std::string> validate_app_json(const nlohmann::json &app) {
    static constexpr std::array<std::string_view, 13> allowed_keys {
      "name"sv,
      "cmd"sv,
      "index"sv,
      "image-path"sv,
      "working-dir"sv,
      "output"sv,
      "prep-cmd"sv,
      "detached"sv,
      "elevated"sv,
      "auto-detach"sv,
      "wait-all"sv,
      "exit-timeout"sv,
      "exclude-global-prep-cmd"sv,
    };

    for (const auto &[key, _] : app.items()) {
      if (std::ranges::find(allowed_keys, key) == allowed_keys.end()) {
        return std::format("Unknown app field '{}'", key);
      }
    }

    if (!app.contains("index") || !app.at("index").is_number_integer()) {
      return "Invalid index";
    }

    if (!app.contains("name") || !app.at("name").is_string()) {
      return "Invalid name";
    }

    if (const auto err = validate_string_field("name", app.at("name").get<std::string>(), MAX_APP_NAME_LENGTH, false)) {
      return err;
    }

    static constexpr std::array<std::string_view, 4> string_fields {
      "cmd"sv,
      "image-path"sv,
      "working-dir"sv,
      "output"sv,
    };

    for (const auto field : string_fields) {
      if (!app.contains(field)) {
        continue;
      }

      if (!app.at(field).is_string()) {
        return std::format("Invalid {}", field);
      }

      if (const auto err = validate_string_field(field, app.at(field).get<std::string>(), MAX_APP_STRING_LENGTH, true)) {
        return err;
      }
    }

    if (app.contains("image-path") && app.at("image-path").is_string()) {
      const auto image_path = app.at("image-path").get<std::string>();
      if (!image_path.empty() && proc::validate_app_image_path(image_path) == proc::default_app_image_path()) {
        return "Invalid image-path";
      }
    }

    static constexpr std::array<std::string_view, 5> bool_fields {
      "elevated"sv,
      "auto-detach"sv,
      "wait-all"sv,
      "exclude-global-prep-cmd"sv,
    };

    for (const auto field : bool_fields) {
      if (app.contains(field) && !app.at(field).is_boolean()) {
        return std::format("Invalid {}", field);
      }
    }

    if (app.contains("exit-timeout") && !app.at("exit-timeout").is_number_integer()) {
      return "Invalid exit-timeout";
    }

    if (app.contains("prep-cmd")) {
      if (!app.at("prep-cmd").is_array()) {
        return "Invalid prep-cmd";
      }

      for (const auto &entry : app.at("prep-cmd")) {
        if (const auto err = validate_prep_cmd_entry(entry)) {
          return err;
        }
      }
    }

    if (app.contains("detached")) {
      if (!app.at("detached").is_array()) {
        return "Invalid detached";
      }

      for (const auto &entry : app.at("detached")) {
        if (!entry.is_string()) {
          return "Invalid detached entry";
        }
        if (const auto err = validate_string_field("detached", entry.get<std::string>(), MAX_APP_STRING_LENGTH, false)) {
          return err;
        }
      }
    }

    return std::nullopt;
  }

  std::optional<std::string> validate_config_patch(const nlohmann::json &patch) {
    const auto &allowlist = config_key_allowlist();

    for (const auto &[key, value] : patch.items()) {
      if (!allowlist.contains(key)) {
        return std::format("Unknown config key '{}'", key);
      }

      if (value.is_null()) {
        continue;
      }

      if (value.is_string()) {
        if (const auto err = validate_string_field(key, value.get<std::string>(), MAX_APP_STRING_LENGTH, true)) {
          return err;
        }
        continue;
      }

      if (key == "global_prep_cmd"sv) {
        if (!value.is_array()) {
          return "Invalid global_prep_cmd";
        }

        for (const auto &entry : value) {
          if (const auto err = validate_prep_cmd_entry(entry)) {
            return err;
          }
        }
      }
    }

    return std::nullopt;
  }

  std::optional<std::string> validate_password_change(const nlohmann::json &input, bool initial_setup) {
    static constexpr std::array<std::string_view, 5> allowed_keys {
      "currentUsername"sv,
      "newUsername"sv,
      "currentPassword"sv,
      "newPassword"sv,
      "confirmNewPassword"sv,
    };

    for (const auto &[key, _] : input.items()) {
      if (std::ranges::find(allowed_keys, key) == allowed_keys.end()) {
        return std::format("Unknown password field '{}'", key);
      }
    }

    const auto validate_username = [](std::string_view username, bool allow_empty) -> std::optional<std::string> {
      if (!allow_empty && username.empty()) {
        return "Invalid Username";
      }
      if (username.size() > MAX_USERNAME_LENGTH) {
        return "Invalid Username";
      }
      if (contains_disallowed_control_chars(username)) {
        return "Invalid Username";
      }
      return std::nullopt;
    };

    const auto validate_password = [](std::string_view password, bool allow_empty) -> std::optional<std::string> {
      if (!allow_empty && password.empty()) {
        return "Invalid Password";
      }
      if (!password.empty() && (password.size() < MIN_PASSWORD_LENGTH || password.size() > MAX_PASSWORD_LENGTH)) {
        return "Invalid Password";
      }
      if (contains_disallowed_control_chars(password)) {
        return "Invalid Password";
      }
      return std::nullopt;
    };

    if (input.contains("currentUsername") && input.at("currentUsername").is_string()) {
      if (const auto err = validate_username(input.at("currentUsername").get<std::string>(), true)) {
        return err;
      }
    }

    if (input.contains("newUsername") && input.at("newUsername").is_string()) {
      if (const auto err = validate_username(input.at("newUsername").get<std::string>(), initial_setup)) {
        return err;
      }
    }

    if (input.contains("currentPassword") && input.at("currentPassword").is_string()) {
      if (const auto err = validate_password(input.at("currentPassword").get<std::string>(), true)) {
        return err;
      }
    }

    if (input.contains("newPassword")) {
      if (!input.at("newPassword").is_string()) {
        return "Invalid Password";
      }
      if (const auto err = validate_password(input.at("newPassword").get<std::string>(), initial_setup)) {
        return err;
      }
    }

    if (input.contains("confirmNewPassword") && input.at("confirmNewPassword").is_string()) {
      if (const auto err = validate_password(input.at("confirmNewPassword").get<std::string>(), true)) {
        return err;
      }
    }

    return std::nullopt;
  }

  bool validate_client_uuid(std::string_view uuid) {
    if (uuid.size() != 36) {
      return false;
    }

    for (std::size_t i = 0; i < uuid.size(); ++i) {
      const char ch = uuid[i];
      if (i == 8 || i == 13 || i == 18 || i == 23) {
        if (ch != '-') {
          return false;
        }
        continue;
      }

      if (!std::isxdigit(static_cast<unsigned char>(ch))) {
        return false;
      }
    }

    return true;
  }

  std::optional<std::string> validate_cover_upload(std::string_view key, std::size_t data_size) {
    if (key.empty()) {
      return "Cover key is required";
    }

    if (key.size() > MAX_COVER_KEY_LENGTH) {
      return "Cover key is too long";
    }

    if (contains_disallowed_control_chars(key)) {
      return "Cover key contains invalid characters";
    }

    if (data_size > MAX_COVER_UPLOAD_BYTES) {
      return "Cover upload is too large";
    }

    return std::nullopt;
  }

  bool is_valid_png_bytes(std::string_view data) {
    static constexpr std::array<unsigned char, 8> PNG_SIGNATURE {
      0x89,
      0x50,
      0x4E,
      0x47,
      0x0D,
      0x0A,
      0x1A,
      0x0A,
    };

    if (data.size() < PNG_SIGNATURE.size()) {
      return false;
    }

    return std::equal(PNG_SIGNATURE.begin(), PNG_SIGNATURE.end(), data.begin(), [](unsigned char expected, char actual) {
      return expected == static_cast<unsigned char>(actual);
    });
  }

  /**
   * @brief Return whether a directory path may be listed by browseDirectory.
   *
   * @param dir_path Candidate directory path from the Web UI.
   * @return False when the path resolves under a blocked system prefix.
   */
  bool is_browse_path_allowed(const fs::path &dir_path) {
    std::error_code ec;
    const fs::path canonical_path = fs::weakly_canonical(dir_path, ec);
    if (ec) {
      return false;
    }

    const auto add_blocked_prefix = [&canonical_path](const fs::path &prefix) -> bool {
      if (prefix.empty()) {
        return false;
      }

      std::error_code prefix_ec;
      const fs::path canonical_prefix = fs::weakly_canonical(prefix, prefix_ec);
      if (prefix_ec) {
        return false;
      }

      return path_has_blocked_prefix(canonical_path, canonical_prefix);
    };

#ifdef _WIN32
    static const std::array<std::wstring, 3> blocked_prefixes {
      L"C:\\Windows\\System32\\config",
      L"C:\\Windows\\System32\\config\\RegBack",
      L"C:\\ProgramData\\Microsoft\\Crypto",
    };

    for (const auto &prefix : blocked_prefixes) {
      if (add_blocked_prefix(prefix)) {
        return false;
      }
    }
#else
    static const std::array<std::string_view, 4> blocked_prefixes {
      "/etc"sv,
      "/proc"sv,
      "/sys"sv,
      "/dev"sv,
    };

    for (const auto prefix : blocked_prefixes) {
      if (add_blocked_prefix(fs::path(prefix))) {
        return false;
      }
    }

    if (const std::string home = lizardbyte::common::get_env("HOME"); !home.empty()) {
      if (add_blocked_prefix(fs::path(home) / ".ssh")) {
        return false;
      }
    }
#endif

    if (!config::sunshine.credentials_file.empty()) {
      const fs::path cred_dir = fs::path(config::sunshine.credentials_file).parent_path();
      if (add_blocked_prefix(cred_dir)) {
        return false;
      }
    }

    if (!config::nvhttp.pkey.empty()) {
      const fs::path pkey_dir = fs::path(config::nvhttp.pkey).parent_path();
      if (add_blocked_prefix(pkey_dir)) {
        return false;
      }
    }

    if (!config::nvhttp.cert.empty()) {
      const fs::path cert_dir = fs::path(config::nvhttp.cert).parent_path();
      if (add_blocked_prefix(cert_dir)) {
        return false;
      }
    }

    return true;
  }

}  // namespace confighttp_validation

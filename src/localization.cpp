/**
 * @file src/localization.cpp
 * @brief Runtime access to localized Web UI strings from C++ code.
 */

// standard includes
#include <filesystem>
#include <fstream>
#include <unordered_map>

// lib includes
#include <nlohmann/json.hpp>

// local includes
#include "assets_path.h"
#include "config.h"
#include "localization.h"
#include "logging.h"

using namespace std::literals;

namespace localization {
  namespace {
    std::unordered_map<std::string, std::string> g_strings;
    bool g_initialized = false;

    /**
     * @brief Build a cache key for a JSON section and entry name.
     *
     * @param section Top-level JSON section.
     * @param key Key within the section.
     * @return Combined lookup key.
     */
    std::string cache_key(const std::string &section, const std::string &key) {
      return section + '.' + key;
    }

    /**
     * @brief Insert string entries from a JSON section into the cache.
     *
     * @param tree Parsed locale JSON.
     * @param section Top-level JSON section to import.
     */
    void import_section(const nlohmann::json &tree, const std::string &section) {
      if (!tree.contains(section) || !tree[section].is_object()) {
        return;
      }

      for (const auto &[key, value] : tree[section].items()) {
        if (!value.is_string()) {
          continue;
        }
        g_strings.emplace(cache_key(section, key), value.get<std::string>());
      }
    }

    /**
     * @brief Load a locale JSON file when it exists.
     *
     * @param locale_path Path to the locale JSON file.
     * @return Parsed JSON tree, or an empty object when unavailable.
     */
    nlohmann::json load_locale_file(const std::filesystem::path &locale_path) {
      std::error_code ec;
      if (!std::filesystem::exists(locale_path, ec)) {
        return nlohmann::json::object();
      }

      std::ifstream locale_file {locale_path};
      if (!locale_file) {
        BOOST_LOG(warning) << "Failed to open locale file: "sv << locale_path.string();
        return nlohmann::json::object();
      }

      try {
        return nlohmann::json::parse(locale_file);
      } catch (const std::exception &ex) {
        BOOST_LOG(warning) << "Failed to parse locale file "sv << locale_path.string() << ": "sv << ex.what();
        return nlohmann::json::object();
      }
    }

    /**
     * @brief Return built-in English defaults for strings used before locale files load.
     *
     * @param section Top-level JSON section.
     * @param key Key within the section.
     * @return Default English label, or an empty string when unknown.
     */
    std::string default_string(const std::string &section, const std::string &key) {
      static const std::unordered_map<std::string, std::string> defaults {
        {cache_key("troubleshooting", "client_notification_name"), "Name"},
        {cache_key("troubleshooting", "client_notification_ip"), "IP"},
        {cache_key("troubleshooting", "client_notification_port"), "Port"},
      };

      const auto it = defaults.find(cache_key(section, key));
      return it == defaults.end() ? std::string {} : it->second;
    }
  }  // namespace

  void init() {
    if (g_initialized) {
      return;
    }
    g_initialized = true;

    const auto locale_dir = std::filesystem::path(assets_path::web()) / "assets" / "locale";
    const auto en_tree = load_locale_file(locale_dir / "en.json");
    import_section(en_tree, "troubleshooting");
    import_section(en_tree, "config");

    if (config::sunshine.locale != "en") {
      const auto locale_tree = load_locale_file(locale_dir / (config::sunshine.locale + ".json"));
      import_section(locale_tree, "troubleshooting");
      import_section(locale_tree, "config");
    }
  }

  std::string ui_string(const std::string &section, const std::string &key) {
    if (!g_initialized) {
      init();
    }

    const auto lookup = cache_key(section, key);
    if (const auto it = g_strings.find(lookup); it != g_strings.end()) {
      return it->second;
    }

    if (section == "troubleshooting") {
      if (key == "client_notification_name") {
        if (const auto name_it = g_strings.find(cache_key("config", "name")); name_it != g_strings.end()) {
          return name_it->second;
        }
      } else if (key == "client_notification_port") {
        if (const auto port_it = g_strings.find(cache_key("config", "port_port")); port_it != g_strings.end()) {
          return port_it->second;
        }
        if (const auto port_it = g_strings.find(cache_key("config", "port")); port_it != g_strings.end()) {
          return port_it->second;
        }
      }
    }

    if (const auto fallback = default_string(section, key); !fallback.empty()) {
      return fallback;
    }

    return key;
  }
}  // namespace localization

/**
 * @file src/localization.h
 * @brief Runtime access to localized Web UI strings from C++ code.
 */
#pragma once

// standard includes
#include <string>

namespace localization {
  /**
   * @brief Load localized strings for the configured Sunshine locale.
   */
  void init();

  /**
   * @brief Look up a localized UI string from the Web UI locale files.
   *
   * @param section Top-level JSON section, such as `troubleshooting`.
   * @param key Key within the section.
   * @return Localized string, falling back to English and then the key name.
   */
  std::string ui_string(const std::string &section, const std::string &key);
}  // namespace localization

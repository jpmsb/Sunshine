/**
 * @file src/assets_path.h
 * @brief Runtime resolution for Sunshine asset directories.
 */
#pragma once

// standard includes
#include <string>

/**
 * @brief Resolve and expose the Sunshine assets directory at runtime.
 */
namespace assets_path {
  /**
   * @brief Initialize the assets directory from the compile-time default and fallbacks.
   */
  void init();

  /**
   * @brief Return the resolved assets root directory.
   *
   * @return Absolute or relative path to the assets directory.
   */
  const std::string &root();

  /**
   * @brief Return the resolved Web UI assets directory.
   *
   * @return Path to the web assets directory, including a trailing slash.
   */
  std::string web();

  /**
   * @brief Join a relative path against the resolved assets root.
   *
   * @param relative Relative path inside the assets directory.
   * @return Full path to the requested asset.
   */
  std::string join(const std::string &relative);
}  // namespace assets_path

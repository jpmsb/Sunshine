/**
 * @file src/platform/linux/portal_options.h
 * @brief Pure helpers for XDG portal / screencast capture options.
 */
#pragma once

// standard includes
#include <cstdint>
#include <string>
#include <string_view>

namespace portal {

  /**
   * @brief Capture backends that share the xdg-desktop-portal PipeWire path.
   */
  enum class capture_mode_e {
    portal,  ///< Legacy portal capture (monitors only, always persist).
    screencast,  ///< Portal UI capture (monitors and windows, optional persist).
  };

  constexpr uint32_t SOURCE_TYPE_MONITOR = 1;  ///< xdg-desktop-portal ScreenCast monitor bit.
  constexpr uint32_t SOURCE_TYPE_WINDOW = 2;  ///< xdg-desktop-portal ScreenCast window bit.

  constexpr uint32_t PERSIST_MODE_WHILE_RUNNING = 1;  ///< Portal persist_mode: restore while the app process runs.
  constexpr uint32_t PERSIST_MODE_UNTIL_REVOKED = 2;  ///< Portal persist_mode: restore until the user revokes.

  constexpr std::string_view PORTAL_TOKEN_FILENAME = "portal_token";  ///< Restore token file for portal mode.
  constexpr std::string_view SCREENCAST_TOKEN_FILENAME = "screencast_token";  ///< Restore token file for screencast mode.

  /**
   * @brief Return the ScreenCast `types` bitmask for a capture mode.
   *
   * @param mode Portal capture mode.
   * @return Bitmask of allowed ScreenCast source types.
   */
  constexpr uint32_t source_types_for_mode(capture_mode_e mode) {
    switch (mode) {
      case capture_mode_e::portal:
        return SOURCE_TYPE_MONITOR;
      case capture_mode_e::screencast:
        return SOURCE_TYPE_MONITOR | SOURCE_TYPE_WINDOW;
    }
    return SOURCE_TYPE_MONITOR;
  }

  /**
   * @brief Return the restore-token filename for a capture mode.
   *
   * @param mode Portal capture mode.
   * @return Token basename under the Sunshine appdata directory.
   */
  constexpr std::string_view token_filename_for_mode(capture_mode_e mode) {
    switch (mode) {
      case capture_mode_e::portal:
        return PORTAL_TOKEN_FILENAME;
      case capture_mode_e::screencast:
        return SCREENCAST_TOKEN_FILENAME;
    }
    return PORTAL_TOKEN_FILENAME;
  }

  /**
   * @brief Decide whether restore-token persistence to disk is enabled for a mode.
   *
   * Portal mode always persists to disk. Screencast mode follows `screencast_persist`.
   *
   * @param mode Portal capture mode.
   * @param screencast_persist User setting for screencast token persistence.
   * @return True when the restore token should be loaded/saved on disk.
   */
  constexpr bool persist_enabled_for_mode(capture_mode_e mode, bool screencast_persist) {
    switch (mode) {
      case capture_mode_e::portal:
        return true;
      case capture_mode_e::screencast:
        return screencast_persist;
    }
    return true;
  }

  /**
   * @brief Return the xdg-desktop-portal `persist_mode` used for ScreenCast restore.
   *
   * Always uses until-revoked so the token remains valid after the portal session is
   * closed (encoder probe resets the display many times). Disk vs in-memory storage
   * is controlled separately by `persist_enabled`.
   *
   * @param persist_to_disk Unused; kept for call-site compatibility.
   * @return Portal persist_mode value.
   */
  constexpr uint32_t persist_mode_for_disk_flag(bool persist_to_disk [[maybe_unused]]) {
    return PERSIST_MODE_UNTIL_REVOKED;
  }

  /**
   * @brief Whether this mode should use a ScreenCast-only portal session.
   *
   * RemoteDesktop sessions cannot use `persist_mode` on some portals (notably KDE),
   * which forces a ScreenCast-only fallback and reopens the picker on every reset.
   *
   * @param mode Portal capture mode.
   * @param persist_to_disk Whether restore tokens are persisted.
   * @param has_restore_token Whether a restore token is already available.
   * @return True when RemoteDesktop should be skipped.
   */
  constexpr bool prefer_screencast_only_session(capture_mode_e mode, bool persist_to_disk, bool has_restore_token) {
    switch (mode) {
      case capture_mode_e::screencast:
        return true;
      case capture_mode_e::portal:
        return persist_to_disk || has_restore_token;
    }
    return true;
  }

  /**
   * @brief Session options passed into the shared portal D-Bus client.
   */
  struct session_options_t {
    uint32_t source_types = SOURCE_TYPE_MONITOR;  ///< ScreenCast source type bitmask.
    bool persist_enabled = true;  ///< Whether to load/save restore tokens on disk.
    std::string token_filename {PORTAL_TOKEN_FILENAME};  ///< Restore token basename under appdata.
    capture_mode_e mode = capture_mode_e::portal;  ///< Capture mode that produced these options.

    /**
     * @brief Build session options for a capture mode and config flag.
     *
     * @param mode Portal capture mode.
     * @param screencast_persist User setting for screencast token persistence.
     * @return Fully populated session options.
     */
    static session_options_t for_mode(capture_mode_e mode, bool screencast_persist) {
      return session_options_t {
        source_types_for_mode(mode),
        persist_enabled_for_mode(mode, screencast_persist),
        std::string {token_filename_for_mode(mode)},
        mode,
      };
    }
  };

}  // namespace portal

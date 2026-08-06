/**
 * @file src/platform/linux/portal_options.h
 * @brief Pure helpers for XDG portal / screencast capture options.
 */
#pragma once

// standard includes
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

// local includes
#include "src/platform/common.h"
#include "src/video.h"

namespace portal {

  /**
   * @brief RGBA color used by the screencast waiting placeholder.
   */
  struct rgba_t {
    std::uint8_t r = 0;  ///< Red channel.
    std::uint8_t g = 0;  ///< Green channel.
    std::uint8_t b = 0;  ///< Blue channel.
    std::uint8_t a = 255;  ///< Alpha channel.
  };

  /**
   * @brief Parse a `#RRGGBB` or `#RRGGBBAA` color string.
   *
   * @param hex Color string from configuration.
   * @return Parsed color, or nullopt when the string is invalid.
   */
  inline std::optional<rgba_t> parse_placeholder_color(std::string_view hex) {
    if (hex.empty() || hex.front() != '#') {
      return std::nullopt;
    }
    hex.remove_prefix(1);
    auto nibble = [](char c) -> int {
      if (c >= '0' && c <= '9') {
        return c - '0';
      }
      if (c >= 'a' && c <= 'f') {
        return 10 + (c - 'a');
      }
      if (c >= 'A' && c <= 'F') {
        return 10 + (c - 'A');
      }
      return -1;
    };
    auto channel = [&](std::size_t i) -> int {
      const int hi = nibble(hex[i]);
      const int lo = nibble(hex[i + 1]);
      if (hi < 0 || lo < 0) {
        return -1;
      }
      return (hi << 4) | lo;
    };

    if (hex.size() != 6 && hex.size() != 8) {
      return std::nullopt;
    }
    const int r = channel(0);
    const int g = channel(2);
    const int b = channel(4);
    if (r < 0 || g < 0 || b < 0) {
      return std::nullopt;
    }
    rgba_t color {
      static_cast<std::uint8_t>(r),
      static_cast<std::uint8_t>(g),
      static_cast<std::uint8_t>(b),
      255,
    };
    if (hex.size() == 8) {
      const int a = channel(6);
      if (a < 0) {
        return std::nullopt;
      }
      color.a = static_cast<std::uint8_t>(a);
    }
    return color;
  }

  /**
   * @brief Resolve placeholder color, falling back to opaque black on invalid input.
   *
   * @param hex Color string from configuration.
   * @return Parsed or default color.
   */
  inline rgba_t placeholder_color_or_default(std::string_view hex) {
    if (auto parsed = parse_placeholder_color(hex)) {
      return *parsed;
    }
    return {};
  }

  /**
   * @brief Resolve placeholder text; empty config uses the hostname.
   *
   * @param configured User-configured text (may be empty).
   * @param hostname Host name used when configured text is empty.
   * @return Text to render on the placeholder.
   */
  inline std::string resolve_placeholder_text(std::string_view configured, std::string_view hostname) {
    if (!configured.empty()) {
      return std::string {configured};
    }
    return std::string {hostname};
  }

  constexpr std::string_view SCREENCAST_PLACEHOLDER_NAME = "screencast-placeholder";  ///< Synthetic display while waiting for portal Start.

  /**
   * @brief Return whether a screencast placeholder font glyph pixel is set.
   *
   * Uses the classic 8x8 bitmap convention where bit 0 (LSB) is the leftmost column.
   *
   * @param ch Character to sample.
   * @param row Glyph row in `[0, 8)`.
   * @param col Glyph column in `[0, 8)`, where 0 is the left edge.
   * @return True when the pixel is set.
   */
  bool placeholder_font_bit(char ch, int row, int col);

  /**
   * @brief Create the screencast waiting placeholder display.
   *
   * @param hwdevice_type Hardware device type requested for capture or encode.
   * @param config Client video configuration.
   * @return Placeholder display backend.
   */
  std::shared_ptr<platf::display_t> make_screencast_placeholder_display(platf::mem_type_e hwdevice_type, const video::config_t &config);

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

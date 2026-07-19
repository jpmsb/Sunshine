/**
 * @file src/system_tray.cpp
 * @brief Definitions for the system tray icon and notification system.
 */
// macros
#if defined SUNSHINE_TRAY && SUNSHINE_TRAY >= 1

  #if defined(_WIN32)
    #define WIN32_LEAN_AND_MEAN  ///< Exclude rarely-used Windows headers from winsock/windows includes.
    #include <accctrl.h>
    #include <aclapi.h>
  #elif defined(__APPLE__) || defined(__MACH__)
    #include <CoreFoundation/CoreFoundation.h>
    #include <dispatch/dispatch.h>
    #include <unordered_map>
  #endif

  // standard includes
  #include <array>
  #include <atomic>
  #include <csignal>
  #include <deque>
  #include <filesystem>
  #include <format>
  #include <memory>
  #include <mutex>
  #include <string>
  #include <thread>
  #include <unordered_map>
  #include <vector>

  // lib includes
  #include <boost/filesystem.hpp>
  #include <boost/process/v1/environment.hpp>
  #include <tray.h>

  // local includes
  #include "assets_path.h"
  #include "config.h"
  #include "display_device.h"
  #include "localization.h"
  #include "logging.h"
  #include "platform/common.h"
  #include "process.h"
  #include "rtsp.h"
  #include "src/entry_handler.h"
  #include "stream.h"

using namespace std::literals;

// system_tray namespace
namespace system_tray {
  static std::atomic tray_initialized = false;

  void refresh_connected_clients_menu();

  void tray_open_ui_cb([[maybe_unused]] struct tray_menu *item) {
    BOOST_LOG(info) << "Opening UI from system tray"sv;
    launch_ui();
  }

  static void tray_left_click_cb([[maybe_unused]] struct tray *item) {
    tray_open_ui_cb(nullptr);
  }

  static void tray_pin_notification_cb() {
    if (config::sunshine.flags.test(config::flag::PIN_STDIN)) {
      return;
    }
    launch_ui("/pin");
  }

  void tray_donate_github_cb([[maybe_unused]] struct tray_menu *item) {
    platf::open_url("https://github.com/sponsors/LizardByte");
  }

  void tray_donate_patreon_cb([[maybe_unused]] struct tray_menu *item) {
    platf::open_url("https://www.patreon.com/LizardByte");
  }

  void tray_donate_paypal_cb([[maybe_unused]] struct tray_menu *item) {
    platf::open_url("https://www.paypal.com/paypalme/ReenigneArcher");
  }

  #if defined(__linux__) || defined(linux) || defined(__linux) || defined(__FreeBSD__)
  /**
   * @brief Forwards Qt log messages to Sunshine's BOOST_LOG logger.
   * @param level Log level: 0=debug, 1=info, 2=warning, 3=error.
   * @param msg The message string from Qt.
   */
  static void qt_log_to_boost(int level, const char *msg) {
    if (msg == nullptr) {
      return;
    }
    switch (level) {
      case 0:
        BOOST_LOG(debug) << "Qt: " << msg;
        break;
      case 1:
        BOOST_LOG(info) << "Qt: " << msg;
        break;
      case 2:
        BOOST_LOG(warning) << "Qt: " << msg;
        break;
      default:
        BOOST_LOG(error) << "Qt: " << msg;
        break;
    }
  }
  #endif

  void tray_reset_display_device_config_cb([[maybe_unused]] struct tray_menu *item) {
    BOOST_LOG(info) << "Resetting display device config from system tray"sv;

    std::ignore = display_device::reset_persistence();
  }

  void tray_restart_cb([[maybe_unused]] struct tray_menu *item) {
    BOOST_LOG(info) << "Restarting from system tray"sv;

    platf::restart();
  }

  void tray_quit_cb([[maybe_unused]] struct tray_menu *item) {
    BOOST_LOG(info) << "Quitting from system tray"sv;

  #ifdef _WIN32
    // If we're running in a service, return a special status to
    // tell it to terminate too, otherwise it will just respawn us.
    if (GetConsoleWindow() == nullptr) {
      lifetime::exit_sunshine(ERROR_SHUTDOWN_IN_PROGRESS, true);
      return;
    }
  #endif

    lifetime::exit_sunshine(0, true);
  }

  /**
   * @brief Tray action metadata stored in tray_menu::context.
   */
  struct tray_session_action_t {
    uint32_t session_id;  ///< Launch-session identifier for the target stream.
    /**
     * @brief Tray action performed when a connected-client menu item is selected.
     */
    enum class type_e {
      pause,  ///< Pause the target session.
      resume,  ///< Resume the target session.
      disconnect,  ///< Disconnect the target session.
    } type;  ///< Action to perform for the menu item.
  };

  static std::mutex tray_menu_mutex;
  static std::vector<std::string> tray_menu_strings;
  static std::vector<std::vector<tray_menu>> tray_session_submenus;
  static std::vector<tray_menu> tray_root_menu;
  static std::vector<std::unique_ptr<tray_session_action_t>> tray_session_action_contexts;

  /**
   * @brief Kind of deferred tray UI work queued for the tray thread.
   */
  enum class tray_pending_kind_e {
    refresh_menu,  ///< Rebuild the tray menu.
    notify_connected,  ///< Show a client-connected notification.
    notify_disconnected,  ///< Show a client-disconnected notification.
    notify_paused,  ///< Show a stream-paused notification.
    notify_resumed,  ///< Show a stream-resumed notification.
    set_streaming_active,  ///< Toggle the streaming-active tray icon state.
    set_pausing_icon,  ///< Switch to the pausing tray icon.
    update_pausing,  ///< Refresh pausing status text/icon.
    update_stopped,  ///< Refresh stopped/idle status text/icon.
    notify_pairing,  ///< Show a pairing-request notification.
    clear_pairing,  ///< Clear pairing notification state.
  };

  /**
   * @brief One deferred tray update waiting to be applied on the tray thread.
   */
  struct tray_pending_item_t {
    tray_pending_kind_e kind;  ///< Type of pending tray work.
    std::string text;  ///< Optional notification or status text payload.
    bool active = false;  ///< Optional boolean flag for icon/state updates.
    uint32_t session_id = 0;  ///< Optional RTSP session id for session actions.
    bool has_session_actions = false;  ///< Whether session pause/disconnect actions apply.
  };

  static std::atomic<bool> tray_has_pending = false;
  static std::mutex tray_pending_mutex;
  static std::deque<tray_pending_item_t> tray_pending_queue;

  static void enqueue_tray_update(
    tray_pending_kind_e kind,
    std::string text = {},
    bool active = false,
    uint32_t session_id = 0,
    bool has_session_actions = false
  ) {
    if (!tray_initialized) {
      return;
    }

    {
      const std::lock_guard lock {tray_pending_mutex};
      tray_pending_queue.push_back({kind, std::move(text), active, session_id, has_session_actions});
    }
    tray_has_pending = true;
  }

  static tray_menu tray_donate_submenu[] = {
    {.text = "GitHub Sponsors", .cb = tray_donate_github_cb},
    {.text = "Patreon", .cb = tray_donate_patreon_cb},
    {.text = "PayPal", .cb = tray_donate_paypal_cb},
    {.text = nullptr}
  };

  // Tray menu instance (declared before dynamic menu rebuild helpers).
  static std::string tray_icon_default;
  static std::string tray_icon_playing;
  static std::string tray_icon_pausing;
  static std::string tray_icon_locked;
  static std::string tray_icon_disconnected;

  static struct tray tray = {
    .icon = nullptr,
    .tooltip = PROJECT_NAME,
    .cb = tray_left_click_cb,
    .menu = nullptr,
    .iconPathCount = 4,
    .allIconPaths = {nullptr, nullptr, nullptr, nullptr},
  };

  #define TRAY_ICON tray_icon_default.c_str()  ///< Default/idle tray icon path.
  #define TRAY_ICON_PLAYING tray_icon_playing.c_str()  ///< Streaming-active tray icon path.
  #define TRAY_ICON_PAUSING tray_icon_pausing.c_str()  ///< Stream-pausing tray icon path.
  #define TRAY_ICON_LOCKED tray_icon_locked.c_str()  ///< PIN-entry/locked tray icon path.
  #define TRAY_ICON_DISCONNECTED tray_icon_disconnected.c_str()  ///< Disconnected tray icon path.

  /**
   * @brief Clear notification fields so tray_update does not re-show stale notifications.
   */
  static void clear_tray_notification_fields() {
    tray.notification_title = nullptr;
    tray.notification_text = nullptr;
    tray.notification_cb = nullptr;
    tray.notification_icon = nullptr;
    tray.notification_actions = nullptr;
  }

  static std::atomic<uint32_t> tray_notification_session_id {0};
  static std::array<tray_notification_action, 3> tray_notification_action_storage {};
  static std::string tray_notification_pause_label;
  static std::string tray_notification_resume_label;
  static std::string tray_notification_disconnect_label;

  static void tray_notification_pause_cb() {
    rtsp_stream::pause_session(tray_notification_session_id.load());
    refresh_connected_clients_menu();
  }

  static void tray_notification_resume_cb() {
    rtsp_stream::resume_session(tray_notification_session_id.load());
    refresh_connected_clients_menu();
  }

  static void tray_notification_disconnect_cb() {
    rtsp_stream::terminate_session(tray_notification_session_id.load());
    refresh_connected_clients_menu();
  }

  /**
   * @brief Configure Pause/Resume and Disconnect actions for a session notification.
   *
   * @param session_id Launch-session identifier targeted by the actions.
   * @param paused True when the session is paused (shows Resume instead of Pause).
   */
  static void configure_session_notification_actions(uint32_t session_id, bool paused) {
    tray_notification_session_id.store(session_id);
    tray_notification_pause_label = localization::ui_string("troubleshooting", "connected_clients_pause");
    tray_notification_resume_label = localization::ui_string("troubleshooting", "connected_clients_resume");
    tray_notification_disconnect_label = localization::ui_string("troubleshooting", "connected_clients_disconnect");

    tray_notification_action_storage[0] = {
      .text = paused ? tray_notification_resume_label.c_str() : tray_notification_pause_label.c_str(),
      .cb = paused ? tray_notification_resume_cb : tray_notification_pause_cb,
    };
    tray_notification_action_storage[1] = {
      .text = tray_notification_disconnect_label.c_str(),
      .cb = tray_notification_disconnect_cb,
    };
    tray_notification_action_storage[2] = {.text = nullptr, .cb = nullptr};

    tray.notification_actions = tray_notification_action_storage.data();
    // Windows balloon notifications only support a single click callback.
    tray.notification_cb = paused ? tray_notification_resume_cb : tray_notification_pause_cb;
  }

#if !defined(_WIN32) && !defined(__APPLE__) && !defined(__MACH__)
  /**
   * @brief Resolve the first existing tray icon path from a candidate list.
   *
   * @param web Web assets root directory.
   * @param candidates Relative icon paths to try in order.
   * @param fallback Relative path used when no candidate exists.
   * @return Absolute path to the selected icon.
   */
  static std::string pick_tray_icon_path(const std::string &web, std::initializer_list<const char *> candidates, const char *fallback) {
    for (const char *candidate : candidates) {
      const auto path = web + candidate;
      if (std::filesystem::exists(path)) {
        return path;
      }
    }

    return web + fallback;
  }
#endif

  /**
   * @brief Resolve tray icon paths from the runtime Web UI assets directory.
   */
  void configure_tray_icon_paths() {
    const auto web = assets_path::web();
  #if defined(_WIN32)
    tray_icon_default = web + "images/sunshine.ico";
    tray_icon_playing = web + "images/sunshine-playing.ico";
    tray_icon_pausing = web + "images/sunshine-pausing.ico";
    tray_icon_locked = web + "images/sunshine-locked.ico";
    tray_icon_disconnected = web + "images/sunshine-disconnected.ico";
  #elif defined(__APPLE__) || defined(__MACH__)
    tray_icon_default = web + "images/logo-sunshine-16.png";
    tray_icon_playing = web + "images/sunshine-playing-16.png";
    tray_icon_pausing = web + "images/sunshine-pausing-16.png";
    tray_icon_locked = web + "images/sunshine-locked-16.png";
    tray_icon_disconnected = web + "images/sunshine-disconnected-16.png";
  #else
    tray_icon_default = pick_tray_icon_path(web, {"images/logo-sunshine.svg", "images/logo-sunshine-45.png", "images/logo-sunshine-16.png"}, "images/logo-sunshine-45.png");
    tray_icon_playing = pick_tray_icon_path(web, {"images/sunshine-playing.svg", "images/sunshine-playing-45.png"}, "images/sunshine-playing.svg");
    tray_icon_pausing = pick_tray_icon_path(web, {"images/sunshine-pausing.svg", "images/sunshine-pausing-45.png"}, "images/sunshine-pausing.svg");
    tray_icon_locked = pick_tray_icon_path(web, {"images/sunshine-locked.svg", "images/sunshine-locked-45.png"}, "images/sunshine-locked.svg");
    tray_icon_disconnected = pick_tray_icon_path(web, {"images/sunshine-disconnected.svg", "images/sunshine-disconnected-45.png"}, "images/sunshine-disconnected.svg");
  #endif

    tray.allIconPaths[0] = tray_icon_default.c_str();
    tray.allIconPaths[1] = tray_icon_locked.c_str();
    tray.allIconPaths[2] = tray_icon_playing.c_str();
    tray.allIconPaths[3] = tray_icon_pausing.c_str();
    tray.icon = tray.allIconPaths[0];
  }

  /**
   * @brief Handle pause, resume, or disconnect actions from a session tray submenu item.
   *
   * @param item Tray menu item whose context points to a tray_session_action_t.
   */
  void tray_session_action_cb(struct tray_menu *item) {
    if (!item || !item->context) {
      return;
    }

    const auto *action = static_cast<tray_session_action_t *>(item->context);

    switch (action->type) {
      case tray_session_action_t::type_e::pause:
        rtsp_stream::pause_session(action->session_id);
        break;
      case tray_session_action_t::type_e::resume:
        rtsp_stream::resume_session(action->session_id);
        break;
      case tray_session_action_t::type_e::disconnect:
        rtsp_stream::terminate_session(action->session_id);
        break;
    }

    refresh_connected_clients_menu();
  }

  static void add_tray_session_action(std::vector<tray_menu> &submenu, const char *text, uint32_t session_id, tray_session_action_t::type_e type) {
    auto action = std::make_unique<tray_session_action_t>(tray_session_action_t {session_id, type});
    submenu.push_back({
      .text = text,
      .cb = tray_session_action_cb,
      .context = action.get(),
    });
    tray_session_action_contexts.push_back(std::move(action));
  }

  /**
   * @brief Rebuild the root tray menu, including connected clients.
   */
  void rebuild_tray_root_menu() {
    tray_menu_strings.clear();
    tray_session_submenus.clear();
    tray_session_action_contexts.clear();
    tray_root_menu.clear();

    const auto sessions = rtsp_stream::list_active_sessions();

    tray_root_menu.push_back({.text = "Open Sunshine", .cb = tray_open_ui_cb});
    tray_root_menu.push_back({.text = "-"});

    tray_menu_strings.emplace_back("Connected Clients");
    tray_root_menu.push_back({
      .text = tray_menu_strings.back().c_str(),
      .disabled = 1,
    });

    if (sessions.empty()) {
      tray_menu_strings.emplace_back("(No clients connected)");
      tray_root_menu.push_back({
        .text = tray_menu_strings.back().c_str(),
        .disabled = 1,
      });
    } else {
      for (const auto &session : sessions) {
        tray_menu_strings.push_back(session.label);
        const auto label_index = tray_menu_strings.size() - 1;
        tray_session_submenus.emplace_back();

        auto &submenu = tray_session_submenus.back();
        tray_menu_strings.emplace_back(session.paused ? "Resume" : "Pause");
        const auto action_label_index = tray_menu_strings.size() - 1;
        tray_menu_strings.emplace_back("Disconnect");
        const auto disconnect_label_index = tray_menu_strings.size() - 1;

        add_tray_session_action(
          submenu,
          tray_menu_strings[action_label_index].c_str(),
          session.session_id,
          session.paused ? tray_session_action_t::type_e::resume : tray_session_action_t::type_e::pause
        );
        add_tray_session_action(
          submenu,
          tray_menu_strings[disconnect_label_index].c_str(),
          session.session_id,
          tray_session_action_t::type_e::disconnect
        );
        submenu.push_back({.text = nullptr});

        tray_root_menu.push_back({
          .text = tray_menu_strings[label_index].c_str(),
          .submenu = submenu.data(),
        });
      }
    }

    tray_root_menu.push_back({.text = "-"});
    tray_root_menu.push_back({.text = "Donate", .submenu = tray_donate_submenu});
    tray_root_menu.push_back({.text = "-"});
  #ifdef _WIN32
    tray_root_menu.push_back({.text = "Reset Display Device Config", .cb = tray_reset_display_device_config_cb});
  #endif
    tray_root_menu.push_back({.text = "Restart", .cb = tray_restart_cb});
    tray_root_menu.push_back({.text = "Quit", .cb = tray_quit_cb});
    tray_root_menu.push_back({.text = nullptr});

    tray.menu = tray_root_menu.data();
  }

  static const char *tray_icon_for_active_sessions(const std::vector<rtsp_stream::active_session_info_t> &sessions) {
    if (sessions.empty()) {
      return TRAY_ICON;
    }

    for (const auto &session : sessions) {
      if (session.paused) {
        return TRAY_ICON_PAUSING;
      }
    }

    return TRAY_ICON_PLAYING;
  }

  static void apply_refresh_connected_clients_menu() {
    const std::lock_guard lock {tray_menu_mutex};
    const auto sessions = rtsp_stream::list_active_sessions();
    rebuild_tray_root_menu();
    tray.icon = tray_icon_for_active_sessions(sessions);
    clear_tray_notification_fields();
    tray_update(&tray);
  }

  static void apply_notify_client_connected(const std::string &body, uint32_t session_id, bool has_session_actions) {
    clear_tray_notification_fields();
    tray_update(&tray);

    static std::string notification_body;
    notification_body = body;
    tray.icon = TRAY_ICON_PLAYING;
    tray.notification_title = "Client Connected";
    tray.notification_text = notification_body.c_str();
    tray.notification_icon = TRAY_ICON_PLAYING;
    if (has_session_actions) {
      configure_session_notification_actions(session_id, false);
    }
    tray_update(&tray);

    clear_tray_notification_fields();
  }

  static void apply_notify_client_disconnected(const std::string &body) {
    clear_tray_notification_fields();
    tray_update(&tray);

    static std::string notification_body;
    notification_body = body;
    tray.icon = TRAY_ICON;
    tray.notification_title = "Client Disconnected";
    tray.notification_text = notification_body.c_str();
    tray.notification_icon = TRAY_ICON_DISCONNECTED;
    tray_update(&tray);

    clear_tray_notification_fields();
  }

  static void apply_notify_client_paused(const std::string &body, uint32_t session_id, bool has_session_actions) {
    clear_tray_notification_fields();
    tray_update(&tray);

    static std::string notification_body;
    notification_body = body;
    tray.icon = TRAY_ICON_PAUSING;
    tray.notification_title = "Client Paused";
    tray.notification_text = notification_body.c_str();
    tray.notification_icon = TRAY_ICON_PAUSING;
    if (has_session_actions) {
      configure_session_notification_actions(session_id, true);
    }
    tray_update(&tray);

    clear_tray_notification_fields();
  }

  static void apply_notify_client_resumed(const std::string &body, uint32_t session_id, bool has_session_actions) {
    clear_tray_notification_fields();
    tray_update(&tray);

    static std::string notification_body;
    notification_body = body;
    tray.icon = TRAY_ICON_PLAYING;
    tray.notification_title = "Client Resumed";
    tray.notification_text = notification_body.c_str();
    tray.notification_icon = TRAY_ICON_PLAYING;
    if (has_session_actions) {
      configure_session_notification_actions(session_id, false);
    }
    tray_update(&tray);

    clear_tray_notification_fields();
  }

  static void apply_notify_pairing_request(const std::string &body) {
    clear_tray_notification_fields();
    tray_update(&tray);

    static std::string notification_title;
    static std::string notification_body;
    notification_title = localization::ui_string("troubleshooting", "pairing_request_title");
    notification_body = body;
    tray.icon = TRAY_ICON_LOCKED;
    tray.notification_title = notification_title.c_str();
    tray.notification_text = notification_body.c_str();
    tray.notification_icon = TRAY_ICON_LOCKED;
    tray.tooltip = PROJECT_NAME;
    if (!config::sunshine.flags.test(config::flag::PIN_STDIN)) {
      tray.notification_cb = tray_pin_notification_cb;
    }
    tray_update(&tray);

    clear_tray_notification_fields();
  }

  static void apply_clear_pairing_request_state() {
    const std::lock_guard lock {tray_menu_mutex};
    const auto sessions = rtsp_stream::list_active_sessions();
    tray.icon = tray_icon_for_active_sessions(sessions);
    clear_tray_notification_fields();
    tray_update(&tray);
  }

  /**
   * @brief Apply tray menu and notification updates on the Qt main thread.
   */
  void process_pending_tray_updates() {
    if (!tray_initialized || !tray_has_pending.exchange(false)) {
      return;
    }

    std::deque<tray_pending_item_t> pending;
    {
      const std::lock_guard lock {tray_pending_mutex};
      pending.swap(tray_pending_queue);
    }

    for (const auto &item : pending) {
      switch (item.kind) {
        case tray_pending_kind_e::refresh_menu:
          apply_refresh_connected_clients_menu();
          break;
        case tray_pending_kind_e::notify_connected:
          apply_notify_client_connected(item.text, item.session_id, item.has_session_actions);
          apply_refresh_connected_clients_menu();
          break;
        case tray_pending_kind_e::notify_disconnected:
          apply_notify_client_disconnected(item.text);
          apply_refresh_connected_clients_menu();
          break;
        case tray_pending_kind_e::notify_paused:
          apply_notify_client_paused(item.text, item.session_id, item.has_session_actions);
          apply_refresh_connected_clients_menu();
          break;
        case tray_pending_kind_e::notify_resumed:
          apply_notify_client_resumed(item.text, item.session_id, item.has_session_actions);
          apply_refresh_connected_clients_menu();
          break;
        case tray_pending_kind_e::set_streaming_active:
          tray.icon = item.active ? TRAY_ICON_PLAYING : TRAY_ICON;
          clear_tray_notification_fields();
          tray_update(&tray);
          break;
        case tray_pending_kind_e::set_pausing_icon:
          tray.icon = TRAY_ICON_PAUSING;
          clear_tray_notification_fields();
          tray_update(&tray);
          break;
        case tray_pending_kind_e::update_pausing: {
          clear_tray_notification_fields();
          tray.icon = TRAY_ICON_PAUSING;
          tray_update(&tray);

          static std::string msg;
          msg = std::format("Streaming paused for {}", item.text);
          tray.icon = TRAY_ICON_PAUSING;
          tray.notification_title = "Stream Paused";
          tray.notification_text = msg.c_str();
          tray.tooltip = msg.c_str();
          tray.notification_icon = TRAY_ICON_PAUSING;
          tray_update(&tray);
          clear_tray_notification_fields();
          break;
        }
        case tray_pending_kind_e::update_stopped: {
          clear_tray_notification_fields();
          tray.icon = TRAY_ICON;
          tray_update(&tray);

          static std::string msg;
          msg = std::format("Application {} successfully stopped", item.text);
          tray.icon = TRAY_ICON;
          tray.notification_icon = TRAY_ICON;
          tray.notification_title = "Application Stopped";
          tray.notification_text = msg.c_str();
          tray.tooltip = PROJECT_NAME;
          tray_update(&tray);
          clear_tray_notification_fields();
          break;
        }
        case tray_pending_kind_e::notify_pairing:
          apply_notify_pairing_request(item.text);
          break;
        case tray_pending_kind_e::clear_pairing:
          apply_clear_pairing_request_state();
          break;
      }
    }
  }

  /**
   * @brief Get resource path.
   *
   * @param relativePath Relative path.
   * @return Absolute path to the resource file for the current platform bundle layout.
   */
  const char *GetResourcePath(const char *relativePath) {
  #ifdef __APPLE__
    if (!relativePath || !*relativePath) {
      return nullptr;
    }

    // Simple cache ensures our string pointers live forever
    static std::unordered_map<std::string, std::string> g_cache;
    auto search = g_cache.find(relativePath);
    if (search != g_cache.end()) {
      return search->second.c_str();
    }

    // If we're running from an .app bundle, get the internal Resources dir
    CFBundleRef bundle = CFBundleGetMainBundle();
    if (!bundle) {
      return relativePath;
    }

    CFURLRef resourcesURL = CFBundleCopyResourcesDirectoryURL(bundle);
    if (!resourcesURL) {
      return relativePath;
    }

    char resourcesPath[PATH_MAX];
    bool ok = CFURLGetFileSystemRepresentation(
      resourcesURL,
      true,
      reinterpret_cast<UInt8 *>(resourcesPath),
      sizeof(resourcesPath)
    );
    CFRelease(resourcesURL);
    if (!ok) {
      return relativePath;
    }

    std::string full;
    if (relativePath && relativePath[0] == '/') {
      full = relativePath;
    } else {
      full = std::string(resourcesPath) + "/" + relativePath;
    }

    BOOST_LOG(debug) << "System Tray: using " << full << " for icon path";

    auto [it, inserted] = g_cache.emplace(relativePath, std::move(full));
    return it->second.c_str();
  #else
    return relativePath;
  #endif
  }

  int init_tray() {
  #ifdef _WIN32
    // If we're running as SYSTEM, Explorer.exe will not have permission to open our thread handle
    // to monitor for thread termination. If Explorer fails to open our thread, our tray icon
    // will persist forever if we terminate unexpectedly. To avoid this, we will modify our thread
    // DACL to add an ACE that allows SYNCHRONIZE access to Everyone.
    {
      PACL old_dacl;
      PSECURITY_DESCRIPTOR sd;
      auto error = GetSecurityInfo(GetCurrentThread(), SE_KERNEL_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, &old_dacl, nullptr, &sd);
      if (error != ERROR_SUCCESS) {
        BOOST_LOG(warning) << "GetSecurityInfo() failed: "sv << error;
        return 1;
      }

      auto free_sd = util::fail_guard([sd]() {
        LocalFree(sd);
      });

      SID_IDENTIFIER_AUTHORITY sid_authority = SECURITY_WORLD_SID_AUTHORITY;
      PSID world_sid;
      if (!AllocateAndInitializeSid(&sid_authority, 1, SECURITY_WORLD_RID, 0, 0, 0, 0, 0, 0, 0, &world_sid)) {
        error = GetLastError();
        BOOST_LOG(warning) << "AllocateAndInitializeSid() failed: "sv << error;
        return 1;
      }

      auto free_sid = util::fail_guard([world_sid]() {
        FreeSid(world_sid);
      });

      EXPLICIT_ACCESS ea {};
      ea.grfAccessPermissions = SYNCHRONIZE;
      ea.grfAccessMode = GRANT_ACCESS;
      ea.grfInheritance = NO_INHERITANCE;
      ea.Trustee.TrusteeForm = TRUSTEE_IS_SID;
      ea.Trustee.ptstrName = (LPSTR) world_sid;

      PACL new_dacl;
      error = SetEntriesInAcl(1, &ea, old_dacl, &new_dacl);
      if (error != ERROR_SUCCESS) {
        BOOST_LOG(warning) << "SetEntriesInAcl() failed: "sv << error;
        return 1;
      }

      auto free_new_dacl = util::fail_guard([new_dacl]() {
        LocalFree(new_dacl);
      });

      error = SetSecurityInfo(GetCurrentThread(), SE_KERNEL_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, new_dacl, nullptr);
      if (error != ERROR_SUCCESS) {
        BOOST_LOG(warning) << "SetSecurityInfo() failed: "sv << error;
        return 1;
      }
    }

    // Wait for the shell to be initialized before registering the tray icon.
    // This ensures the tray icon works reliably after a logoff/logon cycle.
    while (GetShellWindow() == nullptr) {
      Sleep(1000);
    }
  #endif

  #ifdef __APPLE__
    configure_tray_icon_paths();
    tray.allIconPaths[0] = GetResourcePath(TRAY_ICON);
    tray.allIconPaths[1] = GetResourcePath(TRAY_ICON_LOCKED);
    tray.allIconPaths[2] = GetResourcePath(TRAY_ICON_PLAYING);
    tray.allIconPaths[3] = GetResourcePath(TRAY_ICON_PAUSING);

    tray.icon = tray.allIconPaths[0];
  #else
    configure_tray_icon_paths();
  #endif

  #if defined(__linux__) || defined(linux) || defined(__linux) || defined(__FreeBSD__)
    tray_set_log_callback(qt_log_to_boost);
  #endif

    tray_set_app_info(PROJECT_NAME, PROJECT_NAME, PROJECT_FQDN);

    if (tray_init(&tray) < 0) {
      BOOST_LOG(warning) << "Failed to create system tray"sv;
      return 1;
    }

    BOOST_LOG(info) << "System tray created"sv;
    tray_initialized = true;
    rebuild_tray_root_menu();
    tray_update(&tray);
    return 0;
  }

  int process_tray_events() {
    if (!tray_initialized) {
      BOOST_LOG(error) << "System tray is not initialized"sv;
      return 1;
    }

    process_pending_tray_updates();
    return tray_loop(0);
  }

  int end_tray() {
    if (tray_initialized) {
      tray_initialized = false;
      tray_exit();
    }
    return 0;
  }

  void update_tray_playing(std::string app_name) {
    enqueue_tray_update(tray_pending_kind_e::set_streaming_active, std::move(app_name), true);
  }

  void set_tray_streaming_active(bool active) {
    enqueue_tray_update(tray_pending_kind_e::set_streaming_active, {}, active);
  }

  void notify_client_connected(const std::string &name, const std::string &address, uint16_t port, uint32_t session_id) {
    enqueue_tray_update(
      tray_pending_kind_e::notify_connected,
      stream::session::format_client_notification_body(address, port, name, display_device::configured_output_display_name()),
      false,
      session_id,
      true
    );
  }

  void notify_client_disconnected(const std::string &name, const std::string &address, uint16_t port, uint32_t session_id) {
    enqueue_tray_update(
      tray_pending_kind_e::notify_disconnected,
      stream::session::format_client_notification_body(address, port, name, display_device::configured_output_display_name()),
      false,
      session_id,
      false
    );
  }

  void notify_client_paused(const std::string &name, const std::string &address, uint16_t port, uint32_t session_id) {
    enqueue_tray_update(
      tray_pending_kind_e::notify_paused,
      stream::session::format_client_notification_body(address, port, name, display_device::configured_output_display_name()),
      false,
      session_id,
      true
    );
  }

  void notify_client_resumed(const std::string &name, const std::string &address, uint16_t port, uint32_t session_id) {
    enqueue_tray_update(
      tray_pending_kind_e::notify_resumed,
      stream::session::format_client_notification_body(address, port, name, display_device::configured_output_display_name()),
      false,
      session_id,
      true
    );
  }

  void refresh_connected_clients_menu() {
    enqueue_tray_update(tray_pending_kind_e::refresh_menu);
  }

  void set_tray_pausing_icon() {
    enqueue_tray_update(tray_pending_kind_e::set_pausing_icon);
  }

  void update_tray_pausing(std::string app_name) {
    enqueue_tray_update(tray_pending_kind_e::update_pausing, std::move(app_name));
  }

  void update_tray_stopped(std::string app_name) {
    enqueue_tray_update(tray_pending_kind_e::update_stopped, std::move(app_name));
  }

  void notify_pairing_request(const std::string &address, uint16_t port) {
    const auto &action_key = config::sunshine.flags.test(config::flag::PIN_STDIN)
                               ? "pairing_request_action_stdin"sv
                               : "pairing_request_action"sv;
    enqueue_tray_update(
      tray_pending_kind_e::notify_pairing,
      stream::session::format_pairing_request_notification_body(
        address,
        port,
        localization::ui_string("troubleshooting", std::string {action_key})
      )
    );
  }

  void clear_pairing_request_state() {
    enqueue_tray_update(tray_pending_kind_e::clear_pairing);
  }

  // Threading functions available on all platforms
  static void tray_thread_worker() {
    platf::set_thread_name("system_tray");
    BOOST_LOG(info) << "System tray thread started"sv;

    // Initialize the tray in this thread
    if (init_tray() != 0) {
      BOOST_LOG(error) << "Failed to initialize tray in thread"sv;
      return;
    }

    // Main tray event loop
    while (process_tray_events() == 0);

    BOOST_LOG(info) << "System tray thread ended"sv;
  }

  int init_tray_threaded() {
    try {
      auto tray_thread = std::jthread(tray_thread_worker);

      // The tray thread doesn't require strong lifetime management.
      // It will exit asynchronously when tray_exit() is called.
      tray_thread.detach();

      BOOST_LOG(info) << "System tray thread initialized successfully"sv;
      return 0;
    } catch (const std::exception &e) {
      BOOST_LOG(error) << "Failed to create tray thread: " << e.what();
      return 1;
    }
  }

}  // namespace system_tray
#endif

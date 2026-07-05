/**
 * @file src/system_tray.cpp
 * @brief Definitions for the system tray icon and notification system.
 */
// macros
#if defined SUNSHINE_TRAY && SUNSHINE_TRAY >= 1

  #if defined(_WIN32)
    #define WIN32_LEAN_AND_MEAN
    #include <accctrl.h>
    #include <aclapi.h>
  #elif defined(__APPLE__) || defined(__MACH__)
    #include <CoreFoundation/CoreFoundation.h>
    #include <dispatch/dispatch.h>
    #include <unordered_map>
  #endif

  // standard includes
  #include <atomic>
  #include <csignal>
  #include <deque>
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
  #include "display_device.h"
  #include "logging.h"
  #include "platform/common.h"
  #include "process.h"
  #include "rtsp.h"
  #include "src/entry_handler.h"

using namespace std::literals;

// system_tray namespace
namespace system_tray {
  static std::atomic tray_initialized = false;

  void tray_open_ui_cb([[maybe_unused]] struct tray_menu *item) {
    BOOST_LOG(info) << "Opening UI from system tray"sv;
    launch_ui();
  }

  static void tray_left_click_cb([[maybe_unused]] struct tray *item) {
    tray_open_ui_cb(nullptr);
  }

  static void tray_pin_notification_cb() {
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
     * @brief Tray action performed when a connected-client submenu item is selected.
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
  static std::vector<tray_menu> tray_connected_clients_submenu;
  static std::vector<tray_menu> tray_root_menu;
  static std::vector<std::unique_ptr<tray_session_action_t>> tray_session_action_contexts;

  enum class tray_pending_kind_e {
    refresh_menu,
    notify_connected,
    set_streaming_active,
    update_pausing,
    update_stopped,
    require_pin,
  };

  struct tray_pending_item_t {
    tray_pending_kind_e kind;
    std::string text;
    bool active = false;
  };

  static std::atomic<bool> tray_has_pending = false;
  static std::mutex tray_pending_mutex;
  static std::deque<tray_pending_item_t> tray_pending_queue;

  static void enqueue_tray_update(tray_pending_kind_e kind, std::string text = {}, bool active = false) {
    if (!tray_initialized) {
      return;
    }

    {
      const std::lock_guard lock {tray_pending_mutex};
      tray_pending_queue.push_back({kind, std::move(text), active});
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

  static struct tray tray = {
    .icon = nullptr,
    .tooltip = PROJECT_NAME,
    .cb = tray_left_click_cb,
    .menu = nullptr,
    .iconPathCount = 4,
    .allIconPaths = {nullptr, nullptr, nullptr, nullptr},
  };

  #define TRAY_ICON tray_icon_default.c_str()
  #define TRAY_ICON_PLAYING tray_icon_playing.c_str()
  #define TRAY_ICON_PAUSING tray_icon_pausing.c_str()
  #define TRAY_ICON_LOCKED tray_icon_locked.c_str()

  /**
   * @brief Clear notification fields so tray_update does not re-show stale notifications.
   */
  static void clear_tray_notification_fields() {
    tray.notification_title = nullptr;
    tray.notification_text = nullptr;
    tray.notification_cb = nullptr;
    tray.notification_icon = nullptr;
  }

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
  #elif defined(__APPLE__) || defined(__MACH__)
    tray_icon_default = web + "images/logo-sunshine-16.png";
    tray_icon_playing = web + "images/sunshine-playing-16.png";
    tray_icon_pausing = web + "images/sunshine-pausing-16.png";
    tray_icon_locked = web + "images/sunshine-locked-16.png";
  #else
    tray_icon_default = web + "images/logo-sunshine.svg";
    tray_icon_playing = web + "images/sunshine-playing.svg";
    tray_icon_pausing = web + "images/sunshine-pausing.svg";
    tray_icon_locked = web + "images/sunshine-locked.svg";
  #endif

    tray.allIconPaths[0] = tray_icon_default.c_str();
    tray.allIconPaths[1] = tray_icon_locked.c_str();
    tray.allIconPaths[2] = tray_icon_playing.c_str();
    tray.allIconPaths[3] = tray_icon_pausing.c_str();
    tray.icon = tray.allIconPaths[0];
  }

  void refresh_connected_clients_menu();

  /**
   * @brief Handle pause, resume, and disconnect actions from the tray menu.
   *
   * @param item Tray menu item that was activated.
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
    tray_connected_clients_submenu.clear();
    tray_session_action_contexts.clear();

    const auto sessions = rtsp_stream::list_active_sessions();
    if (sessions.empty()) {
      tray_menu_strings.emplace_back("(No clients connected)");
      tray_connected_clients_submenu.push_back({
        .text = tray_menu_strings.back().c_str(),
        .disabled = 1,
      });
    } else {
      for (const auto &session : sessions) {
        tray_menu_strings.push_back(session.label);
        const auto label_index = tray_menu_strings.size() - 1;
        tray_session_submenus.emplace_back();

        auto &submenu = tray_session_submenus.back();
        if (session.paused) {
          tray_menu_strings.emplace_back("Resume");
          add_tray_session_action(submenu, tray_menu_strings.back().c_str(), session.session_id, tray_session_action_t::type_e::resume);
        } else {
          tray_menu_strings.emplace_back("Pause");
          add_tray_session_action(submenu, tray_menu_strings.back().c_str(), session.session_id, tray_session_action_t::type_e::pause);
        }

        tray_menu_strings.emplace_back("Disconnect");
        add_tray_session_action(submenu, tray_menu_strings.back().c_str(), session.session_id, tray_session_action_t::type_e::disconnect);
        submenu.push_back({.text = nullptr});

        tray_connected_clients_submenu.push_back({
          .text = tray_menu_strings[label_index].c_str(),
          .submenu = submenu.data(),
        });
      }
    }

    tray_connected_clients_submenu.push_back({.text = nullptr});

    tray_root_menu = {
      {.text = "Open Sunshine", .cb = tray_open_ui_cb},
      {.text = "-"},
      {.text = "Connected Clients", .submenu = tray_connected_clients_submenu.data()},
      {.text = "-"},
      {.text = "Donate", .submenu = tray_donate_submenu},
      {.text = "-"},
  #ifdef _WIN32
      {.text = "Reset Display Device Config", .cb = tray_reset_display_device_config_cb},
  #endif
      {.text = "Restart", .cb = tray_restart_cb},
      {.text = "Quit", .cb = tray_quit_cb},
      {.text = nullptr},
    };

    tray.menu = tray_root_menu.data();
  }

  static void apply_refresh_connected_clients_menu() {
    const std::lock_guard lock {tray_menu_mutex};
    const auto sessions = rtsp_stream::list_active_sessions();
    rebuild_tray_root_menu();
    tray.icon = sessions.empty() ? TRAY_ICON : TRAY_ICON_PLAYING;
    clear_tray_notification_fields();
    tray_update(&tray);
  }

  static void apply_notify_client_connected(const std::string &label) {
    clear_tray_notification_fields();
    tray_update(&tray);

    static std::string notification_label;
    notification_label = label;
    tray.icon = TRAY_ICON_PLAYING;
    tray.notification_title = "Client Connected";
    tray.notification_text = notification_label.c_str();
    tray.notification_icon = TRAY_ICON_PLAYING;
    tray_update(&tray);

    clear_tray_notification_fields();
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
          apply_notify_client_connected(item.text);
          apply_refresh_connected_clients_menu();
          break;
        case tray_pending_kind_e::set_streaming_active:
          tray.icon = item.active ? TRAY_ICON_PLAYING : TRAY_ICON;
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
        case tray_pending_kind_e::require_pin:
          clear_tray_notification_fields();
          tray.icon = TRAY_ICON;
          tray_update(&tray);
          tray.icon = TRAY_ICON_LOCKED;
          tray.notification_title = "Incoming Pairing Request";
          tray.notification_text = "Click here to complete the pairing process";
          tray.notification_icon = TRAY_ICON_LOCKED;
          tray.tooltip = PROJECT_NAME;
          tray.notification_cb = tray_pin_notification_cb;
          tray_update(&tray);
          clear_tray_notification_fields();
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

  void notify_client_connected(const std::string &label) {
    enqueue_tray_update(tray_pending_kind_e::notify_connected, label);
  }

  void refresh_connected_clients_menu() {
    enqueue_tray_update(tray_pending_kind_e::refresh_menu);
  }

  void update_tray_pausing(std::string app_name) {
    enqueue_tray_update(tray_pending_kind_e::update_pausing, std::move(app_name));
  }

  void update_tray_stopped(std::string app_name) {
    enqueue_tray_update(tray_pending_kind_e::update_stopped, std::move(app_name));
  }

  void update_tray_require_pin() {
    enqueue_tray_update(tray_pending_kind_e::require_pin);
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
      auto tray_thread = std::thread(tray_thread_worker);

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

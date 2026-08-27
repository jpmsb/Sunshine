/**
 * @file src/platform/linux/portalgrab.cpp
 * @brief Definitions for XDG portal grab.
 */
// standard includes
#include <atomic>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

// local includes
#include "config.h"
#include "pipewire.cpp"
#include "portal_options.h"
#include "screencast_placeholder.cpp"
#include "src/globals.h"

namespace {
  // Portal configuration constants
  constexpr uint32_t CURSOR_MODE_EMBEDDED = 2;

  constexpr uint32_t TYPE_KEYBOARD = 1;
  constexpr uint32_t TYPE_POINTER = 2;
  constexpr uint32_t TYPE_TOUCHSCREEN = 4;

  // Portal D-Bus interface names and paths
  constexpr const char *PORTAL_NAME = "org.freedesktop.portal.Desktop";
  constexpr const char *PORTAL_PATH = "/org/freedesktop/portal/desktop";
  constexpr const char *REMOTE_DESKTOP_IFACE = "org.freedesktop.portal.RemoteDesktop";
  constexpr const char *SCREENCAST_IFACE = "org.freedesktop.portal.ScreenCast";
  constexpr const char *REQUEST_IFACE = "org.freedesktop.portal.Request";

  constexpr const char REQUEST_PREFIX[] = "/org/freedesktop/portal/desktop/request/";
  constexpr const char SESSION_PREFIX[] = "/org/freedesktop/portal/desktop/session/";

  // In-memory restore token for screencast when disk persistence is disabled.
  // Allows encoder probe / display resets in the same process to skip the picker when
  // the portal returns a restore_token.
  std::mutex screencast_runtime_token_mutex;
  std::string screencast_runtime_token;

  /**
   * @brief Read the process-local screencast restore token.
   *
   * @return Current in-memory restore token, or empty when unset.
   */
  std::string get_screencast_runtime_token() {
    std::scoped_lock lock(screencast_runtime_token_mutex);
    return screencast_runtime_token;
  }

  /**
   * @brief Store a process-local screencast restore token.
   *
   * @param token Restore token returned by xdg-desktop-portal Start.
   */
  void set_screencast_runtime_token(std::string_view token) {
    std::scoped_lock lock(screencast_runtime_token_mutex);
    screencast_runtime_token = token;
  }

  /**
   * @brief Clear the process-local screencast restore token.
   */
  void clear_screencast_runtime_token() {
    std::scoped_lock lock(screencast_runtime_token_mutex);
    screencast_runtime_token.clear();
  }

  // One-shot: next dbus_t::init skips restore tokens so the system picker opens again.
  // Also suppresses keepalive in ~portal_t while the old grant is being discarded.
  std::atomic_bool screencast_force_fresh_start {false};
  // Suppress keepalive when swapping to a newly committed portal session (mid-stream reselect).
  std::atomic_bool screencast_suppress_keepalive {false};
}  // namespace

using namespace std::literals;

namespace portal {
  /**
   * @brief Persistent portal restore token used to reuse screencast permission.
   */
  class restore_token_t {
  public:
    /**
     * @brief Construct a restore token bound to a basename under appdata.
     *
     * @param filename Token basename such as `portal_token` or `screencast_token`.
     */
    explicit restore_token_t(std::string filename):
        filename_(std::move(filename)) {
    }

    /**
     * @brief Return the currently wrapped value or handle.
     *
     * @return Underlying native handle or object pointer.
     */
    std::string get() const {
      return token_;
    }

    /**
     * @brief Store the new value and mark it dirty for persistence.
     *
     * @param value Portal restore token received from xdg-desktop-portal.
     */
    void set(std::string_view value) {
      token_ = value;
    }

    /**
     * @brief Return whether the persisted value is empty.
     *
     * @return True when no portal display id has been persisted.
     */
    bool empty() const {
      return token_.empty();
    }

    /**
     * @brief Load persisted state from its backing store.
     */
    void load() {
      std::ifstream file(get_file_path());
      if (file.is_open()) {
        std::getline(file, token_);
        if (!token_.empty()) {
          BOOST_LOG(info) << "[portalgrab] Loaded portal restore token from disk ("sv << filename_ << ")"sv;
        }
      }
    }

    /**
     * @brief Save current state to its backing store.
     */
    void save() {
      if (token_.empty()) {
        return;
      }
      std::ofstream file(get_file_path());
      if (file.is_open()) {
        file << token_;
        BOOST_LOG(info) << "[portalgrab] Saved portal restore token to disk ("sv << filename_ << ")"sv;
      } else {
        BOOST_LOG(warning) << "[portalgrab] Failed to save portal restore token ("sv << filename_ << ")"sv;
      }
    }

  private:
    std::string filename_;  ///< Token basename under the Sunshine appdata directory.
    std::string token_;  ///< In-memory restore token value.

    std::string get_file_path() const {
      return platf::appdata().string() + "/" + filename_;
    }
  };

  /**
   * @brief DBus response loop and response variant for portal calls.
   */
  struct dbus_response_t {
    GMainLoop *loop;  ///< GLib main loop waiting for a portal response signal.
    GVariant *response;  ///< DBus response payload returned by the portal.
    guint subscription_id;  ///< Subscription ID.
  };

  /**
   * @brief PipeWire stream node and negotiated capture size.
   */
  struct pipewire_streaminfo_t {
    uint32_t pipewire_node = PW_ID_ANY;  ///< PipeWire node ID selected by the portal.
    uint64_t pipewire_object_serial = SPA_ID_INVALID;  ///< PipeWire object serial selected by the portal.
    int width = 0;  ///< Stream width in pixels.
    int height = 0;  ///< Stream height in pixels.
    int pos_x = 0;  ///< Output X position reported by the portal.
    int pos_y = 0;  ///< Output Y position reported by the portal.
    std::string monitor_name;  ///< Monitor name.

    /**
     * @brief Convert to display name.
     *
     * @return Value converted to display name.
     */
    std::string to_display_name() {
      if (!monitor_name.empty()) {
        return monitor_name;
      }
      // Window streams omit a monitor name and may change size while streaming. Encode a
      // stable id from the PipeWire node so resize does not look like display removal.
      return std::format("screencast-pw-{}", pipewire_node);
    }

    /**
     * @brief Check whether a portal stream matches a requested display name.
     *
     * @param display_name Display name.
     * @return True when the portal display id matches the requested display name.
     */
    bool match_display_name(const std::string_view &display_name) {
      if (display_name.empty()) {
        return false;
      }
      if (display_name == to_display_name()) {
        return true;
      }
      // Accept legacy size-encoded names from earlier builds of this capture path.
      if (monitor_name.empty() && display_name.starts_with("position-")) {
        return true;
      }
      return false;
    }
  };

  /**
   * @brief DBus connection and portal request helpers for screencast setup.
   */
  class dbus_t {
  public:
    /**
     * @brief Construct a D-Bus portal client with the given session options.
     *
     * @param options Capture mode options (source types, persist, token file).
     */
    explicit dbus_t(session_options_t options):
        options_(std::move(options)),
        restore_token_(options_.token_filename) {
    }

    dbus_t &operator=(dbus_t &&) = delete;  // Do not allow to copying

    ~dbus_t() noexcept {
      // Do not use BOOST_LOG here. This destructor can run during process exit after
      // Boost.Log TLS has been torn down (e.g. process-wide screencast_live_dbus),
      // which aborts with "Failed to set TLS value".
      try {
        if (conn && !session_handle.empty()) {
          g_autoptr(GError) err = nullptr;
          // This is a blocking C call; it won't throw, but we wrap for safety
          g_dbus_connection_call_sync(
            conn,
            "org.freedesktop.portal.Desktop",
            session_handle.c_str(),
            "org.freedesktop.portal.Session",
            "Close",
            nullptr,
            nullptr,
            G_DBUS_CALL_FLAGS_NONE,
            -1,
            nullptr,
            &err
          );
          (void) err;
        }
      } catch (...) {
      }

      if (pipewire_fd >= 0) {
        close(pipewire_fd);
      }
      if (screencast_proxy) {
        g_clear_object(&screencast_proxy);
      }
      if (remote_desktop_proxy) {
        g_clear_object(&remote_desktop_proxy);
      }
      if (conn) {
        g_clear_object(&conn);
      }
    }

    /**
     * @brief Open DBus and prepare portal screencast request handling.
     *
     * @return 0 on success; nonzero or negative platform status on failure.
     */
    int init() {
      if (conn) {
        return 0;
      }

      // Tray "Change Capture Source" (and similar) must open the picker even when a
      // restore token exists from the previous grant. Cleared only after a successful Start.
      if (screencast_force_fresh_start.load()) {
        restore_token_.set("");
        BOOST_LOG(info) << "[portalgrab] Forcing fresh screencast Start for source reselect"sv;
      } else if (options_.persist_enabled) {
        restore_token_.load();
      } else if (options_.token_filename == SCREENCAST_TOKEN_FILENAME) {
        // Reuse the in-process token so encoder probe can reset the display without
        // reopening the system picker on every validation pass.
        if (auto runtime_token = get_screencast_runtime_token(); !runtime_token.empty()) {
          restore_token_.set(runtime_token);
          BOOST_LOG(info) << "[portalgrab] Using in-memory screencast restore token"sv;
        }
      }

      conn = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, nullptr);
      if (!conn) {
        return -1;
      }
      remote_desktop_proxy = g_dbus_proxy_new_sync(conn, G_DBUS_PROXY_FLAGS_NONE, nullptr, PORTAL_NAME, PORTAL_PATH, REMOTE_DESKTOP_IFACE, nullptr, nullptr);
      if (!remote_desktop_proxy) {
        return -1;
      }
      screencast_proxy = g_dbus_proxy_new_sync(conn, G_DBUS_PROXY_FLAGS_NONE, nullptr, PORTAL_NAME, PORTAL_PATH, SCREENCAST_IFACE, nullptr, nullptr);
      if (!screencast_proxy) {
        return -1;
      }

      return 0;
    }

    /**
     * @brief Whether a portal session with streams is already available.
     *
     * @return True when Start has completed and streams were published.
     */
    bool ready() const {
      return !session_handle.empty() && !pipewire_streams.empty();
    }

    /**
     * @brief Return the capture mode that created this portal client.
     *
     * @return Portal or screencast capture mode.
     */
    capture_mode_e mode() const {
      return options_.mode;
    }

    /**
     * @brief Connect to xdg-desktop-portal and restore or create a screencast session.
     *
     * @return 0 when a portal session is ready; nonzero when D-Bus or portal setup fails.
     */
    int connect_to_portal() {
      if (ready()) {
        BOOST_LOG(info) << "[portalgrab] Reusing portal session streams without reopening the picker"sv;
        return 0;
      }

      g_autoptr(GMainLoop) loop = g_main_loop_new(nullptr, FALSE);
      g_autofree gchar *session_path = nullptr;
      g_autofree gchar *session_token = nullptr;
      create_session_path(conn, nullptr, &session_token);

      // RemoteDesktop sessions cannot use persist_mode on some portals (e.g. KDE:
      // "Remote desktop sessions cannot persist"). Prefer ScreenCast-only whenever we
      // need restore tokens or the screencast UI capture mode.
      const bool screencast_only = prefer_screencast_only_session(options_.mode, options_.persist_enabled, !restore_token_.empty());
      bool use_screencast_only = screencast_only;
      if (screencast_only) {
        BOOST_LOG(info) << "[portalgrab] Using ScreenCast-only portal session"sv;
      } else {
        use_screencast_only = !try_remote_desktop_session(loop, &session_path, session_token);
      }

      // Fall back to ScreenCast-only if RemoteDesktop failed
      if (use_screencast_only && try_screencast_only_session(loop, &session_path) < 0) {
        return -1;
      }

      if (start_portal_session(loop, session_path, pipewire_streams, use_screencast_only) < 0) {
        return -1;
      }

      if (pipewire_streams.empty()) {
        BOOST_LOG(error) << "[portalgrab] Portal Start returned no usable streams"sv;
        return -1;
      }

      screencast_force_fresh_start.store(false);
      return 0;
    }

    /**
     * @brief Open a PipeWire remote FD for the active portal session.
     *
     * @param fd Set to the newly opened PipeWire FD on success.
     * @return 0 on success; nonzero when OpenPipeWireRemote fails.
     */
    int acquire_pipewire_fd(int &fd) {
      if (session_handle.empty()) {
        return -1;
      }
      return open_pipewire_remote(session_handle.c_str(), fd);
    }

    // Try to create a combined RemoteDesktop + ScreenCast session
    // Returns true on success, false if should fall back to ScreenCast-only
    /**
     * @brief Try to create a RemoteDesktop portal session.
     *
     * @param loop GLib main loop associated with the portal request.
     * @param session_path Session path.
     * @param session_token Session token.
     * @return True when the portal request or state check succeeds.
     */
    bool try_remote_desktop_session(GMainLoop *loop, gchar **session_path, const gchar *session_token) {
      if (create_portal_session(loop, session_path, session_token, false) < 0) {
        return false;
      }

      if (select_remote_desktop_devices(loop, *session_path) < 0) {
        BOOST_LOG(warning) << "[portalgrab] RemoteDesktop.SelectDevices failed, falling back to ScreenCast-only mode"sv;
        g_free(*session_path);
        *session_path = nullptr;
        return false;
      }

      if (select_screencast_sources(loop, *session_path, false) < 0) {
        BOOST_LOG(warning) << "[portalgrab] ScreenCast.SelectSources failed with RemoteDesktop session, trying ScreenCast-only mode"sv;
        g_free(*session_path);
        *session_path = nullptr;
        return false;
      }

      return true;
    }

    // Create a ScreenCast-only session
    /**
     * @brief Create a screencast-only portal session without remote-desktop control.
     *
     * @param loop GLib main loop associated with the portal request.
     * @param session_path Session path.
     * @return 0 when the portal returns a session path; nonzero on request failure.
     */
    int try_screencast_only_session(GMainLoop *loop, gchar **session_path) {
      g_autofree gchar *new_session_token = nullptr;
      create_session_path(conn, nullptr, &new_session_token);
      if (create_portal_session(loop, session_path, new_session_token, true) < 0) {
        return -1;
      }
      if (select_screencast_sources(loop, *session_path, true) < 0) {
        g_free(*session_path);
        *session_path = nullptr;
        return -1;
      }
      return 0;
    }

    /**
     * @brief Check whether session closed.
     *
     * @return True when the portal session has been closed.
     */
    bool is_session_closed() const {
      if (conn && !session_handle.empty()) {
        // Try to retrieve property org.freedesktop.portal.Session::version
        g_autoptr(GError) err = nullptr;
        g_dbus_connection_call_sync(
          conn,
          "org.freedesktop.portal.Desktop",
          session_handle.c_str(),
          "org.freedesktop.DBus.Properties",
          "Get",
          g_variant_new("(ss)", "org.freedesktop.portal.Session", "version"),
          G_VARIANT_TYPE("(v)"),
          G_DBUS_CALL_FLAGS_NONE,
          -1,
          nullptr,
          &err
        );
        // If we cannot get the property then the session portal was closed.
        if (err) {
          BOOST_LOG(debug) << "[portalgrab] Session closed as check failed: "sv << err->message;
          return true;
        }
      }
      // The session is not closed (or might not have been opened yet).
      return false;
    }

    std::vector<pipewire_streaminfo_t> pipewire_streams;  ///< Pipewire streams.
    int pipewire_fd {-1};  ///< Pipewire fd.

  private:
    session_options_t options_;  ///< Capture options for this portal session.
    restore_token_t restore_token_;  ///< Restore token bound to the session token file.
    GDBusConnection *conn {nullptr};  ///< Session bus connection for portal calls.
    GDBusProxy *screencast_proxy {nullptr};  ///< Proxy for the ScreenCast portal interface.
    GDBusProxy *remote_desktop_proxy {nullptr};  ///< Proxy for the RemoteDesktop portal interface.
    std::string session_handle;  ///< Active portal session object path.

    int create_portal_session(GMainLoop *loop, gchar **session_path_out, const gchar *session_token, bool use_screencast) {
      GDBusProxy *proxy = use_screencast ? screencast_proxy : remote_desktop_proxy;
      const char *session_type = use_screencast ? "ScreenCast" : "RemoteDesktop";

      dbus_response_t response = {
        nullptr,
      };
      g_autofree gchar *request_token = nullptr;
      create_request_path(conn, nullptr, &request_token);

      GVariantBuilder builder;
      g_variant_builder_init(&builder, G_VARIANT_TYPE("(a{sv})"));
      g_variant_builder_open(&builder, G_VARIANT_TYPE("a{sv}"));
      g_variant_builder_add(&builder, "{sv}", "handle_token", g_variant_new_string(request_token));
      g_variant_builder_add(&builder, "{sv}", "session_handle_token", g_variant_new_string(session_token));
      g_variant_builder_close(&builder);

      g_autoptr(GError) err = nullptr;
      g_autoptr(GVariant) reply = g_dbus_proxy_call_sync(proxy, "CreateSession", g_variant_builder_end(&builder), G_DBUS_CALL_FLAGS_NONE, -1, nullptr, &err);

      if (err) {
        BOOST_LOG(error) << "[portalgrab] Could not create "sv << session_type << " session: "sv << err->message;
        return -1;
      }

      const gchar *request_path = nullptr;
      g_variant_get(reply, "(o)", &request_path);
      dbus_response_init(&response, loop, conn, request_path);

      g_autoptr(GVariant) create_response = dbus_response_wait(&response);

      if (!create_response) {
        BOOST_LOG(error) << "[portalgrab] " << session_type << " CreateSession: no response received"sv;
        return -1;
      }

      guint32 response_code;
      g_autoptr(GVariant) results = nullptr;
      g_variant_get(create_response, "(u@a{sv})", &response_code, &results);

      BOOST_LOG(debug) << "[portalgrab] " << session_type << " CreateSession response_code: "sv << response_code;

      if (response_code != 0) {
        BOOST_LOG(error) << "[portalgrab] " << session_type << " CreateSession failed with response code: "sv << response_code;
        return -1;
      }

      g_autoptr(GVariant) session_handle_v = g_variant_lookup_value(results, "session_handle", nullptr);
      if (!session_handle_v) {
        BOOST_LOG(error) << "[portalgrab] " << session_type << " CreateSession: session_handle not found in response"sv;
        return -1;
      }

      if (g_variant_is_of_type(session_handle_v, G_VARIANT_TYPE_VARIANT)) {
        g_autoptr(GVariant) inner = g_variant_get_variant(session_handle_v);
        *session_path_out = g_strdup(g_variant_get_string(inner, nullptr));
      } else {
        *session_path_out = g_strdup(g_variant_get_string(session_handle_v, nullptr));
      }

      BOOST_LOG(debug) << "[portalgrab] " << session_type << " CreateSession: got session handle: "sv << *session_path_out;
      // Save it for the destructor to use during cleanup
      this->session_handle = *session_path_out;
      return 0;
    }

    int select_remote_desktop_devices(GMainLoop *loop, const gchar *session_path) {
      dbus_response_t response = {
        nullptr,
      };
      g_autofree gchar *request_token = nullptr;
      create_request_path(conn, nullptr, &request_token);

      GVariantBuilder builder;
      g_variant_builder_init(&builder, G_VARIANT_TYPE("(oa{sv})"));
      g_variant_builder_add(&builder, "o", session_path);
      g_variant_builder_open(&builder, G_VARIANT_TYPE("a{sv}"));
      g_variant_builder_add(&builder, "{sv}", "handle_token", g_variant_new_string(request_token));
      g_variant_builder_add(&builder, "{sv}", "types", g_variant_new_uint32(TYPE_KEYBOARD | TYPE_POINTER | TYPE_TOUCHSCREEN));
      // Do not send persist_mode on RemoteDesktop: some portals reject it with
      // "Remote desktop sessions cannot persist" and force a ScreenCast-only fallback.
      g_variant_builder_close(&builder);

      g_autoptr(GError) err = nullptr;
      g_autoptr(GVariant) reply = g_dbus_proxy_call_sync(remote_desktop_proxy, "SelectDevices", g_variant_builder_end(&builder), G_DBUS_CALL_FLAGS_NONE, -1, nullptr, &err);

      if (err) {
        BOOST_LOG(error) << "[portalgrab] Could not select devices: "sv << err->message;
        return -1;
      }

      const gchar *request_path = nullptr;
      g_variant_get(reply, "(o)", &request_path);
      dbus_response_init(&response, loop, conn, request_path);

      g_autoptr(GVariant) devices_response = dbus_response_wait(&response);

      if (!devices_response) {
        BOOST_LOG(error) << "[portalgrab] SelectDevices: no response received"sv;
        return -1;
      }

      guint32 response_code;
      g_variant_get(devices_response, "(u@a{sv})", &response_code, nullptr);
      BOOST_LOG(debug) << "[portalgrab] SelectDevices response_code: "sv << response_code;

      if (response_code != 0) {
        BOOST_LOG(error) << "[portalgrab] SelectDevices failed with response code: "sv << response_code;
        return -1;
      }

      return 0;
    }

    /**
     * @brief Request ScreenCast sources for an existing portal session.
     *
     * @param loop GLib main loop associated with the portal request.
     * @param session_path Portal session object path.
     * @param allow_persist Whether to send persist_mode/restore_token (ScreenCast-only only).
     * @return 0 on success; nonzero when SelectSources fails.
     */
    int select_screencast_sources(GMainLoop *loop, const gchar *session_path, bool allow_persist) {
      dbus_response_t response = {
        nullptr,
      };
      g_autofree gchar *request_token = nullptr;
      create_request_path(conn, nullptr, &request_token);

      GVariantBuilder builder;
      g_variant_builder_init(&builder, G_VARIANT_TYPE("(oa{sv})"));
      g_variant_builder_add(&builder, "o", session_path);
      g_variant_builder_open(&builder, G_VARIANT_TYPE("a{sv}"));
      g_variant_builder_add(&builder, "{sv}", "handle_token", g_variant_new_string(request_token));
      g_variant_builder_add(&builder, "{sv}", "types", g_variant_new_uint32(options_.source_types));
      g_variant_builder_add(&builder, "{sv}", "cursor_mode", g_variant_new_uint32(CURSOR_MODE_EMBEDDED));
      g_variant_builder_add(&builder, "{sv}", "multiple", g_variant_new_boolean(TRUE));
      // Persist/restore is only valid on ScreenCast-only sessions on some portals (e.g. KDE).
      if (allow_persist) {
        g_variant_builder_add(&builder, "{sv}", "persist_mode", g_variant_new_uint32(persist_mode_for_disk_flag(options_.persist_enabled)));
        if (!restore_token_.empty()) {
          g_variant_builder_add(&builder, "{sv}", "restore_token", g_variant_new_string(restore_token_.get().c_str()));
        }
      }
      g_variant_builder_close(&builder);

      g_autoptr(GError) err = nullptr;
      g_autoptr(GVariant) reply = g_dbus_proxy_call_sync(screencast_proxy, "SelectSources", g_variant_builder_end(&builder), G_DBUS_CALL_FLAGS_NONE, -1, nullptr, &err);
      if (err) {
        BOOST_LOG(error) << "[portalgrab] Could not select sources: "sv << err->message;
        return -1;
      }

      const gchar *request_path = nullptr;
      g_variant_get(reply, "(o)", &request_path);
      dbus_response_init(&response, loop, conn, request_path);

      g_autoptr(GVariant) sources_response = dbus_response_wait(&response);

      if (!sources_response) {
        BOOST_LOG(error) << "[portalgrab] SelectSources: no response received"sv;
        return -1;
      }

      guint32 response_code;
      g_variant_get(sources_response, "(u@a{sv})", &response_code, nullptr);
      BOOST_LOG(debug) << "[portalgrab] SelectSources response_code: "sv << response_code;

      if (response_code != 0) {
        BOOST_LOG(error) << "[portalgrab] SelectSources failed with response code: "sv << response_code;
        return -1;
      }

      return 0;
    }

    int start_portal_session(GMainLoop *loop, const gchar *session_path, std::vector<pipewire_streaminfo_t> &out_pipewire_streams, bool use_screencast) {
      GDBusProxy *proxy = use_screencast ? screencast_proxy : remote_desktop_proxy;
      const char *session_type = use_screencast ? "ScreenCast" : "RemoteDesktop";

      dbus_response_t response = {
        nullptr,
      };
      g_autofree gchar *request_token = nullptr;
      create_request_path(conn, nullptr, &request_token);

      GVariantBuilder builder;
      g_variant_builder_init(&builder, G_VARIANT_TYPE("(osa{sv})"));
      g_variant_builder_add(&builder, "o", session_path);
      g_variant_builder_add(&builder, "s", "");  // parent_window
      g_variant_builder_open(&builder, G_VARIANT_TYPE("a{sv}"));
      g_variant_builder_add(&builder, "{sv}", "handle_token", g_variant_new_string(request_token));
      g_variant_builder_close(&builder);

      g_autoptr(GError) err = nullptr;
      g_autoptr(GVariant) reply = g_dbus_proxy_call_sync(proxy, "Start", g_variant_builder_end(&builder), G_DBUS_CALL_FLAGS_NONE, -1, nullptr, &err);
      if (err) {
        BOOST_LOG(error) << "[portalgrab] Could not start "sv << session_type << " session: "sv << err->message;
        return -1;
      }

      const gchar *request_path = nullptr;
      g_variant_get(reply, "(o)", &request_path);
      dbus_response_init(&response, loop, conn, request_path);

      g_autoptr(GVariant) start_response = dbus_response_wait(&response);

      if (!start_response) {
        BOOST_LOG(error) << "[portalgrab] " << session_type << " Start: no response received"sv;
        return -1;
      }

      guint32 response_code;
      g_autoptr(GVariant) dict = nullptr;
      g_autoptr(GVariant) streams = nullptr;
      g_variant_get(start_response, "(u@a{sv})", &response_code, &dict);

      BOOST_LOG(info) << "[portalgrab] " << session_type << " Start response_code: "sv << response_code;

      if (response_code != 0) {
        BOOST_LOG(error) << "[portalgrab] " << session_type << " Start failed with response code: "sv << response_code;
        return -1;
      }

      streams = g_variant_lookup_value(dict, "streams", G_VARIANT_TYPE("a(ua{sv})"));
      if (!streams) {
        BOOST_LOG(error) << "[portalgrab] " << session_type << " Start: no streams in response"sv;
        return -1;
      }

      if (const gchar *new_token = nullptr; g_variant_lookup(dict, "restore_token", "s", &new_token) && new_token && new_token[0] != '\0' && restore_token_.get() != new_token) {
        restore_token_.set(new_token);
        if (options_.persist_enabled) {
          restore_token_.save();
        } else if (options_.token_filename == SCREENCAST_TOKEN_FILENAME) {
          set_screencast_runtime_token(new_token);
          BOOST_LOG(info) << "[portalgrab] Cached in-memory screencast restore token"sv;
        }
      } else if (options_.mode == capture_mode_e::screencast) {
        // Host apps without a portal app-id often get no restore_token from KDE.
        // The live session cache in portal_t covers reuse within this process.
        BOOST_LOG(info) << "[portalgrab] Start response had no restore_token; relying on live session reuse"sv;
      }

      GVariantIter iter;
      const auto wl_monitors = wl::monitors();
      uint32_t out_pipewire_node;
      g_autoptr(GVariant) value = nullptr;
      g_variant_iter_init(&iter, streams);
      while (g_variant_iter_next(&iter, "(u@a{sv})", &out_pipewire_node, &value)) {
        int out_width = 0;
        int out_height = 0;
        bool result = g_variant_lookup(value, "size", "(ii)", &out_width, &out_height, nullptr);
        uint32_t source_type = 0;
        g_variant_lookup(value, "source_type", "u", &source_type);
        // `size` is optional in the ScreenCast.Start API. Window streams (source_type=2)
        // from some portals (notably KDE) omit it; PipeWire negotiates the real dimensions.
        if (!result) {
          BOOST_LOG(info) << "[portalgrab] Stream on pipewire node "sv << out_pipewire_node
                          << " has no size property (source_type="sv << source_type
                          << "); deferring resolution to PipeWire negotiation"sv;
          out_width = 0;
          out_height = 0;
        }

        int out_pos_x;
        int out_pos_y;
        result = g_variant_lookup(value, "position", "(ii)", &out_pos_x, &out_pos_y, nullptr);
        if (!result) {
          BOOST_LOG(warning) << "[portalgrab] Falling back to position 0x0 for stream with resolution "sv << out_width << "x"sv << out_height << "on pipewire node "sv << out_pipewire_node;
          out_pos_x = 0;
          out_pos_y = 0;
        }

        uint64_t out_pipewire_object_serial;
        result = g_variant_lookup(value, "pipewire-serial", "t", &out_pipewire_object_serial);
        if (!result) {
          // If pipewire-serial was not present explicitly set to invalid value.
          out_pipewire_object_serial = SPA_ID_INVALID;
        }

        auto stream = pipewire_streaminfo_t {out_pipewire_node, out_pipewire_object_serial, out_width, out_height, out_pos_x, out_pos_y};

        // Try to match the stream to a monitor_name by position/resolution and update stream info
        for (const auto &monitor : wl_monitors) {
          if (!monitor) {
            continue;
          }
          if (monitor->viewport.offset_x == out_pos_x && monitor->viewport.offset_y == out_pos_y && monitor->viewport.logical_width == out_width && monitor->viewport.logical_height == out_height) {
            stream.monitor_name = monitor->name;
            break;
          }
        }


        out_pipewire_streams.emplace_back(stream);
      }

      // The portal call returns the streams sorted by out_pipewire_node which can shuffle displays around, so
      // we have to sort pipewire streams by position here to be consistent
      std::ranges::sort(out_pipewire_streams, [](const auto &a, const auto &b) {
        return a.pos_x < b.pos_x || a.pos_y < b.pos_y;
      });


      return 0;
    }

    int open_pipewire_remote(const gchar *session_path, int &fd) {
      g_autoptr(GUnixFDList) fd_list = nullptr;
      g_autoptr(GVariant) msg = g_variant_ref_sink(g_variant_new("(oa{sv})", session_path, nullptr));

      g_autoptr(GError) err = nullptr;
      g_autoptr(GVariant) reply = g_dbus_proxy_call_with_unix_fd_list_sync(screencast_proxy, "OpenPipeWireRemote", msg, G_DBUS_CALL_FLAGS_NONE, -1, nullptr, &fd_list, nullptr, &err);
      if (err) {
        BOOST_LOG(error) << "[portalgrab] Could not open pipewire remote: "sv << err->message;
        return -1;
      }

      int fd_handle;
      g_variant_get(reply, "(h)", &fd_handle);
      fd = g_unix_fd_list_get(fd_list, fd_handle, nullptr);
      return 0;
    }

    static void on_response_received_cb([[maybe_unused]] GDBusConnection *connection, [[maybe_unused]] const gchar *sender_name, [[maybe_unused]] const gchar *object_path, [[maybe_unused]] const gchar *interface_name, [[maybe_unused]] const gchar *signal_name, GVariant *parameters, gpointer user_data) {
      auto *response = static_cast<dbus_response_t *>(user_data);
      response->response = g_variant_ref_sink(parameters);
      g_main_loop_quit(response->loop);
    }

    static gchar *get_sender_string(GDBusConnection *conn) {
      gchar *sender = g_strdup(g_dbus_connection_get_unique_name(conn) + 1);
      gchar *dot;
      while ((dot = strstr(sender, ".")) != nullptr) {
        *dot = '_';
      }
      return sender;
    }

    static void create_request_path(GDBusConnection *conn, gchar **out_path, gchar **out_token) {
      static uint32_t request_count = 0;

      request_count++;

      if (out_token) {
        *out_token = g_strdup_printf("Sunshine%u", request_count);
      }
      if (out_path) {
        g_autofree gchar *sender = get_sender_string(conn);
        *out_path = g_strdup(std::format("{}{}{}{}", REQUEST_PREFIX, sender, "/Sunshine", request_count).c_str());
      }
    }

    static void create_session_path(GDBusConnection *conn, gchar **out_path, gchar **out_token) {
      static uint32_t session_count = 0;

      session_count++;

      if (out_token) {
        *out_token = g_strdup_printf("Sunshine%u", session_count);
      }

      if (out_path) {
        g_autofree gchar *sender = get_sender_string(conn);
        *out_path = g_strdup(std::format("{}{}{}{}", SESSION_PREFIX, sender, "/Sunshine", session_count).c_str());
      }
    }

    static void dbus_response_init(struct dbus_response_t *response, GMainLoop *loop, GDBusConnection *conn, const char *request_path) {
      response->loop = loop;
      response->subscription_id = g_dbus_connection_signal_subscribe(conn, PORTAL_NAME, REQUEST_IFACE, "Response", request_path, nullptr, G_DBUS_SIGNAL_FLAGS_NONE, on_response_received_cb, response, nullptr);
    }

    static GVariant *dbus_response_wait(struct dbus_response_t *response) {
      g_main_loop_run(response->loop);
      return response->response;
    }
  };

  /**
   * @brief Process-wide live ScreenCast session for screencast capture mode.
   *
   * Encoder probe and client connect each create a display backend. Without a
   * restore_token from the portal, closing the session between those steps forces
   * the system picker again. Keep one dbus_t alive for the process lifetime.
   */
  std::mutex screencast_live_mutex;
  std::shared_ptr<dbus_t> screencast_live_dbus;  ///< Process-wide live ScreenCast D-Bus session.
  std::shared_ptr<dbus_t> screencast_pending_shadow;  ///< Shadow session awaiting capture-thread commit.
  std::shared_ptr<dbus_t> screencast_old_session_to_close;  ///< Previous live session closed after swap.

  std::mutex screencast_picker_mutex;  ///< Serialize bootstrap and shadow pickers.
  std::atomic_bool screencast_bootstrap_stop {false};  ///< Set when the boot-time ScreenCast picker thread should exit.
  std::thread screencast_bootstrap_thread;  ///< Thread that runs the boot-time ScreenCast portal picker.
  std::atomic_bool screencast_reselect_running {false};  ///< True while a mid-stream shadow reselect picker is active.

  /**
   * @brief Idle PipeWire consumer that keeps the portal node alive between clients.
   *
   * KDE tears down the ScreenCast PipeWire node when the last consumer disconnects.
   * Holding a lightweight memptr consumer lets reconnect reuse the live D-Bus session
   * without reopening the picker or attaching to a dead node.
   */
  struct screencast_keepalive_t {
    std::mutex mutex;  ///< Guards keepalive PipeWire consumer state.
    std::shared_ptr<dbus_t> session;  ///< Portal session whose PipeWire node is held open.
    std::unique_ptr<pipewire::pipewire_t> pipewire;  ///< Idle PipeWire consumer for the portal node.
    std::shared_ptr<pipewire::shared_state_t> shared;  ///< Shared PipeWire state for the keepalive consumer.
    uint32_t node = 0;  ///< PipeWire node ID currently held by the keepalive consumer.
  };

  screencast_keepalive_t screencast_keepalive;  ///< Process-wide PipeWire keepalive for the live portal node.

  /**
   * @brief Raise mail::screencast_ready when the mail bus is available.
   */
  void raise_screencast_ready() {
    if (mail::man) {
      mail::man->event<bool>(mail::screencast_ready)->raise(true);
    }
  }

  /**
   * @brief Whether a shadow screencast session is waiting to be committed.
   *
   * @return True when begin_screencast_source_reselect() succeeded and apply is pending.
   */
  bool has_pending_screencast_swap() {
    std::scoped_lock lock(screencast_live_mutex);
    return static_cast<bool>(screencast_pending_shadow);
  }

  /**
   * @brief Whether a live screencast portal session is ready for PipeWire capture.
   *
   * @return True when the process-wide live session has streams.
   */
  bool has_screencast_live_session() {
    std::scoped_lock lock(screencast_live_mutex);
    return screencast_live_dbus && !screencast_live_dbus->is_session_closed() && screencast_live_dbus->ready();
  }

  /**
   * @brief Stop the process-wide screencast PipeWire keepalive consumer.
   */
  void stop_screencast_keepalive() {
    std::unique_ptr<pipewire::pipewire_t> dropping_pw;
    std::shared_ptr<pipewire::shared_state_t> dropping_shared;
    std::shared_ptr<dbus_t> dropping_session;
    {
      std::scoped_lock lock(screencast_keepalive.mutex);
      if (!screencast_keepalive.pipewire) {
        return;
      }
      BOOST_LOG(info) << "[portalgrab] Stopping screencast PipeWire keepalive"sv;
      dropping_pw = std::move(screencast_keepalive.pipewire);
      dropping_shared = std::move(screencast_keepalive.shared);
      dropping_session = std::move(screencast_keepalive.session);
      screencast_keepalive.node = 0;
    }
    // Close the portal session before tearing PipeWire. After a sibling consumer on the
    // same grant was destroyed, pw_stream_destroy on the keepalive SIGSEGVs; closing the
    // session first and disconnecting the core avoids that path.
    dropping_session.reset();
    dropping_shared.reset();
    if (dropping_pw) {
      dropping_pw->destroy(pipewire::pipewire_t::destroy_stream_e::via_core);
    }
    dropping_pw.reset();
  }

  /**
   * @brief Ensure an idle PipeWire consumer is attached to the live screencast node.
   *
   * Uses a lightweight memptr consumer only between clients. Display init stops this keepalive
   * immediately before ensure_stream (see on_before_ensure_stream) so the streaming consumer
   * can negotiate DMA-BUF alone. Encode still gates VAAPI VRAM on `negotiated_dmabuf`.
   *
   * @param session Live screencast D-Bus session.
   * @param node PipeWire node id from ScreenCast Start.
   * @param width Last known width (1 if unknown).
   * @param height Last known height (1 if unknown).
   * @return 0 on success or when keepalive already holds the node; -1 on failure.
   */
  int ensure_screencast_keepalive(std::shared_ptr<dbus_t> session, uint32_t node, int width, int height) {
    if (!session || !session->ready() || node == 0 || node == PW_ID_ANY) {
      return -1;
    }

    {
      std::scoped_lock lock(screencast_keepalive.mutex);
      if (screencast_keepalive.pipewire && screencast_keepalive.session == session && screencast_keepalive.node == node) {
        const bool dead = screencast_keepalive.shared && screencast_keepalive.shared->stream_dead.load();
        if (!dead) {
          return 0;
        }
        BOOST_LOG(warning) << "[portalgrab] Screencast keepalive stream is dead; recreating"sv;
      }
    }
    stop_screencast_keepalive();

    int fd = -1;
    if (session->acquire_pipewire_fd(fd) < 0 || fd < 0) {
      BOOST_LOG(warning) << "[portalgrab] Keepalive OpenPipeWireRemote failed"sv;
      return -1;
    }

    auto pw = std::make_unique<pipewire::pipewire_t>();
    auto shared = std::make_shared<pipewire::shared_state_t>();
    if (pw->init(fd, node, SPA_ID_INVALID, shared) < 0) {
      BOOST_LOG(warning) << "[portalgrab] Keepalive pipewire init failed"sv;
      close(fd);
      return -1;
    }

    const uint32_t ensure_w = width > 0 ? static_cast<uint32_t>(width) : 1u;
    const uint32_t ensure_h = height > 0 ? static_cast<uint32_t>(height) : 1u;
    // Memptr-only: cheap to keep the portal node alive between clients.
    // ensure_stream expects AVRational after fractional/variable-rate PipeWire support.
    if (pw->ensure_stream(platf::mem_type_e::system, ensure_w, ensure_h, AVRational {60, 1}, nullptr, 0, false) < 0) {
      BOOST_LOG(warning) << "[portalgrab] Keepalive ensure_stream failed"sv;
      pw->destroy();
      return -1;
    }

    {
      std::scoped_lock lock(screencast_keepalive.mutex);
      screencast_keepalive.session = std::move(session);
      screencast_keepalive.pipewire = std::move(pw);
      screencast_keepalive.shared = std::move(shared);
      screencast_keepalive.node = node;
    }
    BOOST_LOG(info) << "[portalgrab] Screencast PipeWire keepalive active for node "sv << node << " (memptr)"sv;
    return 0;
  }

  /**
   * @brief Acquire a D-Bus portal client, reusing the live screencast session when possible.
   *
   * @param options Capture mode options.
   * @return Shared portal D-Bus client.
   */
  std::shared_ptr<dbus_t> acquire_portal_dbus(session_options_t options) {
    if (options.mode == capture_mode_e::screencast) {
      std::shared_ptr<dbus_t> dropping;
      {
        std::scoped_lock lock(screencast_live_mutex);
        if (screencast_live_dbus) {
          if (!screencast_live_dbus->is_session_closed() && screencast_live_dbus->ready()) {
            BOOST_LOG(info) << "[portalgrab] Reusing live screencast portal session"sv;
            return screencast_live_dbus;
          }
          BOOST_LOG(info) << "[portalgrab] Dropping inactive screencast portal session"sv;
          dropping = std::move(screencast_live_dbus);
        }
      }
      if (dropping) {
        stop_screencast_keepalive();
        dropping.reset();
      }
    }

    return std::make_shared<dbus_t>(std::move(options));
  }

  /**
   * @brief Publish a successful screencast portal session for later reuse.
   *
   * @param session Connected screencast D-Bus client with active streams.
   */
  void publish_screencast_live_session(std::shared_ptr<dbus_t> session) {
    if (!session || !session->ready() || session->mode() != capture_mode_e::screencast) {
      return;
    }
    std::scoped_lock lock(screencast_live_mutex);
    if (screencast_live_dbus != session) {
      BOOST_LOG(info) << "[portalgrab] Publishing live screencast portal session for reuse"sv;
      screencast_live_dbus = std::move(session);
    }
  }

  /**
   * @brief Drop the process-wide screencast session when the compositor closed it.
   *
   * @param which Session instance that observed the closure.
   */
  void clear_screencast_live_session(const dbus_t *which) {
    stop_screencast_keepalive();
    std::scoped_lock lock(screencast_live_mutex);
    if (screencast_live_dbus.get() == which) {
      BOOST_LOG(info) << "[portalgrab] Clearing live screencast portal session"sv;
      screencast_live_dbus.reset();
    }
  }

  /**
   * @brief Release the process-wide screencast portal session before logging teardown.
   *
   * Must be called from platform deinit while Boost.Log is still usable. Leaving the
   * session for static destruction runs ~dbus_t after TLS shutdown and aborts on Ctrl+C.
   */
  void release_screencast_live_session() {
    stop_screencast_keepalive();
    std::shared_ptr<dbus_t> dropping;
    {
      std::scoped_lock lock(screencast_live_mutex);
      if (!screencast_live_dbus) {
        return;
      }
      BOOST_LOG(info) << "[portalgrab] Releasing live screencast portal session on shutdown"sv;
      dropping = std::move(screencast_live_dbus);
    }
    // Destroy outside the mutex so Close() cannot deadlock with other portal work.
    dropping.reset();
  }

  /**
   * @brief Tear down the live screencast grant so the next capture opens a fresh picker.
   *
   * Used when no client is streaming. With active clients, prefer begin_screencast_source_reselect().
   */
  void request_screencast_source_reselect() {
    BOOST_LOG(info) << "[portalgrab] Requesting screencast source reselect (fresh portal picker)"sv;
    screencast_force_fresh_start.store(true);
    clear_screencast_runtime_token();
    {
      std::scoped_lock lock(screencast_live_mutex);
      if (screencast_live_dbus) {
        BOOST_LOG(info) << "[portalgrab] Detaching live screencast cache for source reselect"sv;
        screencast_live_dbus.reset();
      }
      screencast_pending_shadow.reset();
      screencast_old_session_to_close.reset();
    }
  }

  /**
   * @brief Drop keepalive after the active streaming consumer has been destroyed.
   *
   * Call only after `disp.reset()` when clearing an idle grant (no clients).
   */
  void finish_screencast_source_reselect() {
    BOOST_LOG(info) << "[portalgrab] Finishing screencast source reselect; stopping keepalive"sv;
    stop_screencast_keepalive();
  }

  /**
   * @brief Run portal CreateSession/SelectSources/Start for screencast (blocking).
   *
   * @param force_fresh When true, ignore restore tokens and open the system picker.
   * @return Ready D-Bus session, or nullptr on cancel/failure.
   */
  std::shared_ptr<dbus_t> run_screencast_portal_picker(bool force_fresh) {
    std::scoped_lock picker_lock(screencast_picker_mutex);
    if (force_fresh) {
      screencast_force_fresh_start.store(true);
      clear_screencast_runtime_token();
    }
    auto options = session_options_t::for_mode(capture_mode_e::screencast, config::video.screencast_persist);
    auto dbus = std::make_shared<dbus_t>(std::move(options));
    if (dbus->init() < 0) {
      BOOST_LOG(warning) << "[portalgrab] Screencast picker: D-Bus init failed"sv;
      return nullptr;
    }
    if (dbus->connect_to_portal() < 0) {
      BOOST_LOG(warning) << "[portalgrab] Screencast picker cancelled or failed"sv;
      return nullptr;
    }
    if (!dbus->ready()) {
      return nullptr;
    }
    return dbus;
  }

  /**
   * @brief Start the boot-time ScreenCast picker on a background thread.
   */
  void screencast_bootstrap_start() {
    screencast_bootstrap_stop.store(false);
    if (screencast_bootstrap_thread.joinable()) {
      return;
    }
    screencast_bootstrap_thread = std::thread([]() {
      BOOST_LOG(info) << "[portalgrab] Starting screencast bootstrap picker"sv;
      if (screencast_bootstrap_stop.load()) {
        return;
      }
      // xdg-desktop-portal denies CreateSession when the caller is non-dumpable or still
      // holds elevated capabilities ("Unable to open /proc/<pid>/root"). Capture mode
      // "screencast" never enables KMS, so dropping CAP_SYS_ADMIN/SYS_NICE here is safe
      // (same sequence as make_portal_display()).
      if (platf::has_elevated_privileges(true)) {
        platf::drop_elevated_privileges(true);
      }
      // Reuse an existing live session (e.g. persist restore already done).
      if (has_screencast_live_session()) {
        BOOST_LOG(info) << "[portalgrab] Screencast live session already present; skipping bootstrap picker"sv;
        raise_screencast_ready();
        return;
      }
      auto dbus = run_screencast_portal_picker(false);
      if (screencast_bootstrap_stop.load() || !dbus) {
        return;
      }
      publish_screencast_live_session(dbus);
      BOOST_LOG(info) << "[portalgrab] Screencast bootstrap picker completed"sv;
      raise_screencast_ready();
    });
  }

  /**
   * @brief Stop the bootstrap thread during platform teardown.
   */
  void screencast_bootstrap_stop_join() {
    screencast_bootstrap_stop.store(true);
    if (screencast_bootstrap_thread.joinable()) {
      screencast_bootstrap_thread.join();
    }
  }

  /**
   * @brief Open a shadow portal picker without tearing down the active capture session.
   */
  void begin_screencast_source_reselect() {
    bool expected = false;
    if (!screencast_reselect_running.compare_exchange_strong(expected, true)) {
      BOOST_LOG(info) << "[portalgrab] Screencast reselect already in progress"sv;
      return;
    }
    std::thread([]() {
      BOOST_LOG(info) << "[portalgrab] Starting shadow screencast source reselect"sv;
      // Same portal /proc/<pid>/root requirement as bootstrap (see screencast_bootstrap_start).
      if (platf::has_elevated_privileges(true)) {
        platf::drop_elevated_privileges(true);
      }
      auto dbus = run_screencast_portal_picker(true);
      if (!dbus) {
        screencast_reselect_running.store(false);
        return;
      }
      {
        std::scoped_lock lock(screencast_live_mutex);
        screencast_pending_shadow = std::move(dbus);
      }
      BOOST_LOG(info) << "[portalgrab] Shadow screencast session ready; signaling capture swap"sv;
      raise_screencast_ready();
      screencast_reselect_running.store(false);
    }).detach();
  }

  /**
   * @brief Commit a pending shadow session (or no-op when bootstrap already published live).
   *
   * Call from the capture thread before destroying the current display during a screencast_ready reinit.
   *
   * @return True when an old session must be closed after `disp.reset()` via finish_screencast_session_swap().
   */
  bool apply_screencast_ready() {
    std::shared_ptr<dbus_t> pending;
    {
      std::scoped_lock lock(screencast_live_mutex);
      pending = std::move(screencast_pending_shadow);
      if (!pending) {
        return false;
      }
      screencast_old_session_to_close = std::move(screencast_live_dbus);
      screencast_live_dbus = std::move(pending);
    }
    // Prevent ~portal_t from attaching keepalive to the session being discarded.
    // Keepalive teardown waits until after disp.reset() in finish_screencast_session_swap().
    screencast_suppress_keepalive.store(true);
    BOOST_LOG(info) << "[portalgrab] Applied pending screencast session for capture swap"sv;
    return true;
  }

  /**
   * @brief Finish a mid-stream session swap after the old display consumer was destroyed.
   */
  void finish_screencast_session_swap() {
    stop_screencast_keepalive();
    std::shared_ptr<dbus_t> dropping;
    {
      std::scoped_lock lock(screencast_live_mutex);
      dropping = std::move(screencast_old_session_to_close);
    }
    dropping.reset();
    screencast_suppress_keepalive.store(false);
    screencast_force_fresh_start.store(false);
  }

  /**
   * @brief Portal screencast backend that negotiates PipeWire streams over DBus.
   */
  class portal_t: public pipewire::pipewire_display_t {
  public:
    /**
     * @brief Construct a portal display backend with the given session options.
     *
     * @param options Capture mode options (source types, persist, token file).
     */
    explicit portal_t(session_options_t options):
        dbus(acquire_portal_dbus(std::move(options))) {
    }

    /**
     * @brief Destroy the PipeWire consumer without necessarily closing the portal session.
     *
     * Screencast mode keeps a process-wide portal session so encoder probe and client
     * streaming can share one picker grant. Portal mode releases the last shared_ptr and
     * closes the session as before.
     */
    ~portal_t() override {
      // Persist last negotiated geometry on the live screencast session so the next
      // display rebuild (e.g. window resize reinit) does not reconnect with 0x0.
      if (dbus && width > 0 && height > 0) {
        for (auto &stream : dbus->pipewire_streams) {
          stream.width = width;
          stream.height = height;
        }
      }
      // Keep the portal PipeWire node alive across client disconnects / display rebuilds.
      // Attach keepalive before destroying this display's consumer so the node never hits
      // zero consumers (KDE would tear it down and the next connect would SIGSEGV).
      // Skip while a source reselect/swap is pending — the old grant is being discarded.
      if (!screencast_force_fresh_start.load() && !screencast_suppress_keepalive.load() && dbus && dbus->mode() == capture_mode_e::screencast && dbus->ready() && !dbus->pipewire_streams.empty()) {
        const auto &stream = dbus->pipewire_streams.front();
        ensure_screencast_keepalive(dbus, stream.pipewire_node, width > 0 ? width : stream.width, height > 0 ? height : stream.height);
      }
      // Portal FDs on PipeWire 1.6.x have SIGSEGV'd in pw_stream_destroy during encoder-probe
      // teardowns; release the stream via core disconnect instead.
      pipewire.destroy(pipewire::pipewire_t::destroy_stream_e::via_core);
      // Screencast keeps the process-wide D-Bus session (and keepalive) so reconnect and
      // additional clients reuse the same picker grant without reopening the UI.
      dbus.reset();
    }

    /**
     * @brief Configure the PipeWire stream through the XDG desktop portal.
     *
     * @param display_name Display name to capture.
     * @param out_pipewire_fd PipeWire file descriptor for the stream, or -1 for local context.
     * @param out_pipewire_node PipeWire node ID, or PW_ID_ANY when using object serial.
     * @param out_pipewire_object_serial PipeWire object serial for the stream.
     * @return 0 when the stream was configured successfully.
     */
    int configure_stream(const std::string &display_name, int &out_pipewire_fd, uint32_t &out_pipewire_node, uint64_t &out_pipewire_object_serial [[maybe_unused]]) override {
      if (!dbus) {
        BOOST_LOG(error) << "[portalgrab] Missing portal D-Bus client. portal_t setup failed.";
        return -1;
      }

      // Connect DBus portal session (no-op when reusing a live screencast session).
      if (dbus->init() < 0) {
        BOOST_LOG(error) << "[portalgrab] Failed to connect to dbus. portal_t setup failed.";
        return -1;
      }
      if (dbus->connect_to_portal() < 0) {
        BOOST_LOG(error) << "[portalgrab] Failed to connect to portal. portal_t setup failed.";
        return -1;
      }

      // Match display_name to a stream from the pipewire_streams vector
      bool use_fallback = true;
      pipewire_streaminfo_t stream;
      auto streams = dbus->pipewire_streams;
      if (streams.empty()) {
        BOOST_LOG(error) << "[portalgrab] No streams found on portal. portal_t setup failed.";
        return -1;
      }
      for (auto &stream_ : streams) {
        if (stream_.match_display_name(display_name)) {
          stream = stream_;
          use_fallback = false;
          break;
        }
      }
      // Fall back to first stream if we cannot match the given display_name to a stream in currently available streams.
      if (use_fallback) {
        BOOST_LOG(info) << "[portalgrab] Using first available stream as no matching stream was found for: '"sv << display_name << "'";
        stream = dbus->pipewire_streams.at(0);
      }

      // Restore global maxframerate negotiation state
      pipewire.set_negotiate_maxframerate(negotiate_maxframerate.load());

      // Each display consumer needs its own PipeWire FD from the live portal session.
      if (dbus->acquire_pipewire_fd(out_pipewire_fd) < 0) {
        BOOST_LOG(error) << "[portalgrab] Failed to open PipeWire remote for portal session.";
        return -1;
      }
      out_pipewire_node = stream.pipewire_node;
      out_pipewire_object_serial = stream.pipewire_object_serial;
      // Set/update basic stream parameters on display_t
      this->offset_x = stream.pos_x;
      this->offset_y = stream.pos_y;
      this->width = stream.width;
      this->height = stream.height;
      this->logical_width = 0;  // Explicitly mark for pipewire_display_t to try to figure this out.
      this->logical_height = 0;  // Explicitly Mark for pipewire_display_t to try to figure this out.

      // Keep screencast sessions alive across probe/stream display rebuilds.
      publish_screencast_live_session(dbus);
      return 0;
    }

    /**
     * @brief Check stream dead.
     *
     * @param out_status Out status.
     * @return True when the PipeWire stream can no longer produce frames.
     */
    bool check_stream_dead(platf::capture_e &out_status) override {
      // If the pipewire stream stopped due to closed portal session stop the capture with an error
      if (dbus && dbus->is_session_closed()) {
        BOOST_LOG(warning) << "[portalgrab] PipeWire stream stopped by closed portal session."sv;
        clear_screencast_live_session(dbus.get());
        pipewire.frame_cv().notify_all();
        out_status = platf::capture_e::error;
        return true;  // Stop capture with error (due to out_status)
      }
      // Disable maxframerate negotiation if the stream died without having ever started (e.g. GNOME mutter does not support it)
      if (shared_state->previous_state != PW_STREAM_STATE_STREAMING && negotiate_maxframerate.load()) {
        BOOST_LOG(warning) << "[portalgrab] Negotiation failed, will retry without maxFramerate"sv;
        negotiate_maxframerate.store(false);
        pipewire.set_negotiate_maxframerate(false);
        out_status = platf::capture_e::reinit;
        return true;  // Stop capture with reinit (due to out_status)
      }
      return false;  // Return to default stream dead handling
    }

    /**
     * @brief Run portal capture and mark that a real streaming session used this display.
     *
     * @param push_captured_image_cb Callback that accepts a newly captured image.
     * @param pull_free_image_cb Callback that provides an available image buffer.
     * @param cursor Cursor.
     * @return Capture status reported to the streaming pipeline.
     */
    platf::capture_e capture(const push_captured_image_cb_t &push_captured_image_cb, const pull_free_image_cb_t &pull_free_image_cb, bool *cursor) override {
      capture_started_ = true;
      return pipewire::pipewire_display_t::capture(push_captured_image_cb, pull_free_image_cb, cursor);
    }

    /**
     * @brief Drop idle keepalive immediately before PipeWire connect in init()/capture().
     *
     * Stopping only in capture() was too late: init() already called ensure_stream while the
     * memptr keepalive was attached, so reconnect negotiated MemPtr and stuck there (ensure
     * is a no-op once the stream exists). That path caused multi-second black on quick reconnect.
     */
    void on_before_ensure_stream() override {
      stop_screencast_keepalive();
    }

    std::shared_ptr<dbus_t> dbus;  ///< DBus portal client (process-shared for screencast mode).
    bool capture_started_ = false;  ///< True after capture() runs (excludes encoder-only probes).

    // Class variable to store runtime state of maxFramerate negotiation
    static inline std::atomic<bool> negotiate_maxframerate {true};  ///< Whether portal negotiation should request the maximum frame rate.
  };

  /**
   * @brief Check whether the xdg-desktop-portal ScreenCast interface is available.
   *
   * This does not create a portal session or show the system picker.
   *
   * @return True when a ScreenCast D-Bus proxy can be created on the session bus.
   */
  bool portal_screencast_interface_available() {
    g_autoptr(GError) err = nullptr;
    g_autoptr(GDBusProxy) proxy = g_dbus_proxy_new_for_bus_sync(
      G_BUS_TYPE_SESSION,
      static_cast<GDBusProxyFlags>(G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES | G_DBUS_PROXY_FLAGS_DO_NOT_CONNECT_SIGNALS),
      nullptr,
      PORTAL_NAME,
      PORTAL_PATH,
      SCREENCAST_IFACE,
      nullptr,
      &err
    );
    if (!proxy) {
      if (err) {
        BOOST_LOG(debug) << "[portalgrab] ScreenCast portal unavailable: "sv << err->message;
      }
      return false;
    }
    return true;
  }

  /**
   * @brief Create a portal-backed display for the given capture mode.
   *
   * @param mode Portal or screencast capture mode.
   * @param hwdevice_type Hardware device type requested for capture or encode.
   * @param display_name Display name.
   * @param config Configuration values to apply.
   * @return Display backend, or nullptr when initialization fails.
   */
  std::shared_ptr<platf::display_t> make_portal_display(capture_mode_e mode, platf::mem_type_e hwdevice_type, const std::string &display_name, const video::config_t &config) {
    using enum platf::mem_type_e;
    if (!pipewire::pipewire_display_t::init_pipewire_and_check_hwdevice_type(hwdevice_type)) {
      BOOST_LOG(error) << "[portalgrab] Could not initialize pipewire-based display with the given hw device type."sv;
      return nullptr;
    }

    // Drop CAP_SYS_ADMIN, CAP_SYS_NICE and set DUMPABLE flag to allow XDG /root access
    if (platf::has_elevated_privileges(true)) {
      platf::drop_elevated_privileges(true);
    }

    auto options = session_options_t::for_mode(mode, config::video.screencast_persist);
    auto portal = std::make_shared<portal_t>(std::move(options));
    if (portal->init(hwdevice_type, display_name, config)) {
      // Live screencast sessions can outlive the PipeWire consumer. After disconnect the
      // portal node is often gone; drop the cache so the next attempt opens a fresh session.
      if (mode == capture_mode_e::screencast && portal->dbus) {
        BOOST_LOG(warning) << "[portalgrab] Clearing live screencast session after capture init failure"sv;
        clear_screencast_live_session(portal->dbus.get());
      }
      return nullptr;
    }

    if (portal->dbus) {
      for (auto &stream : portal->dbus->pipewire_streams) {
        if (stream.match_display_name(display_name) || portal->dbus->pipewire_streams.size() == 1) {
          stream.width = portal->width;
          stream.height = portal->height;
        }
      }
    }

    return portal;
  }

  /**
   * @brief Enumerate portal streams for the given capture mode.
   *
   * @param mode Portal or screencast capture mode.
   * @return Display names, or an empty list when discovery fails.
   */
  std::vector<std::string> portal_display_names_for_mode(capture_mode_e mode) {
    std::vector<std::string> display_names;

    if (platf::has_elevated_privileges(true)) {
      // We're still in the probing phase of Sunshine startup. Dropping portal security early will break KMS.
      // Just return a dummy screen for now. Display re-enumeration after encoder probing will yield full result.
      display_names.emplace_back("init");
      return display_names;
    }

    // Screencast: never block the capture/enumerate path on the system picker. Bootstrap owns that.
    if (mode == capture_mode_e::screencast) {
      std::shared_ptr<dbus_t> live;
      {
        std::scoped_lock lock(screencast_live_mutex);
        live = screencast_live_dbus;
      }
      if (live && !live->is_session_closed() && live->ready()) {
        for (auto stream_ : live->pipewire_streams) {
          BOOST_LOG(info) << "[portalgrab] Found stream for display id/name: '"sv << stream_.monitor_name << "' position: "sv << stream_.pos_x << "x"sv << stream_.pos_y << " resolution: "sv << stream_.width << "x"sv << stream_.height;
          display_names.emplace_back(stream_.to_display_name());
        }
        if (!display_names.empty()) {
          return display_names;
        }
      }
      display_names.emplace_back(std::string {SCREENCAST_PLACEHOLDER_NAME});
      return display_names;
    }

    auto options = session_options_t::for_mode(mode, config::video.screencast_persist);
    auto dbus = acquire_portal_dbus(std::move(options));

    if (dbus->init() < 0) {
      BOOST_LOG(warning) << "[portalgrab] Failed to connect to dbus. Cannot enumerate displays, returning empty list.";
      return {};
    }

    if (dbus->connect_to_portal() < 0) {
      BOOST_LOG(warning) << "[portalgrab] Failed to connect to portal. Cannot enumerate displays, returning empty list.";
      return {};
    }

    publish_screencast_live_session(dbus);

    for (auto stream_ : dbus->pipewire_streams) {
      BOOST_LOG(info) << "[portalgrab] Found stream for display id/name: '"sv << stream_.monitor_name << "' position: "sv << stream_.pos_x << "x"sv << stream_.pos_y << " resolution: "sv << stream_.width << "x"sv << stream_.height;
      display_names.emplace_back(stream_.to_display_name());
    }
    // Keep screencast sessions alive via the live-session cache. Portal mode releases when dbus goes out of scope.

    // Return currently active display names
    return display_names;
  }
}  // namespace portal

namespace platf {
  /**
   * @brief Create a portal-based display capture backend.
   *
   * @param hwdevice_type Hardware device type requested for capture or encode.
   * @param display_name Display name.
   * @param config Configuration values to apply.
   * @return Display backend backed by xdg-desktop-portal and PipeWire, or nullptr.
   */
  std::shared_ptr<display_t> portal_display(mem_type_e hwdevice_type, const std::string &display_name, const video::config_t &config) {
    return portal::make_portal_display(portal::capture_mode_e::portal, hwdevice_type, display_name, config);
  }

  /**
   * @brief Create a screencast portal display capture backend with system source picker.
   *
   * @param hwdevice_type Hardware device type requested for capture or encode.
   * @param display_name Display name.
   * @param config Configuration values to apply.
   * @return Display backend backed by xdg-desktop-portal ScreenCast UI and PipeWire, or nullptr.
   */
  std::shared_ptr<display_t> screencast_display(mem_type_e hwdevice_type, const std::string &display_name, const video::config_t &config) {
    if (!portal::has_screencast_live_session()) {
      BOOST_LOG(info) << "[portalgrab] Using screencast placeholder display"sv;
      return portal::make_screencast_placeholder_display(hwdevice_type, config);
    }
    return portal::make_portal_display(portal::capture_mode_e::screencast, hwdevice_type, display_name, config);
  }

  /**
   * @brief Enumerate capture targets available through xdg-desktop-portal.
   *
   * @return Portal display names, or an empty list when portal discovery fails.
   */
  std::vector<std::string> portal_display_names() {
    return portal::portal_display_names_for_mode(portal::capture_mode_e::portal);
  }

  /**
   * @brief Enumerate capture targets available through the screencast portal UI.
   *
   * @return Screencast display names, or an empty list when portal discovery fails.
   */
  std::vector<std::string> screencast_display_names() {
    return portal::portal_display_names_for_mode(portal::capture_mode_e::screencast);
  }

  /**
   * @brief Check whether the screencast portal backend can be used without opening the picker.
   *
   * @return True when the ScreenCast portal D-Bus interface is available.
   */
  bool screencast_available() {
    return portal::portal_screencast_interface_available();
  }
}  // namespace platf

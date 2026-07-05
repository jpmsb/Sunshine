/**
 * @file src/assets_path.cpp
 * @brief Runtime resolution for Sunshine asset directories.
 */

// standard includes
#include <filesystem>
#include <vector>

// platform includes
#ifdef __linux__
  #include <climits>
  #include <unistd.h>
#endif

// local includes
#include "assets_path.h"
#include "logging.h"

using namespace std::literals;

namespace assets_path {
  namespace {
    std::string g_root;
    bool g_initialized = false;

    /**
     * @brief Return the directory containing the current executable.
     *
     * @return Executable directory, or an empty string when unavailable.
     */
    std::string executable_dir() {
#ifdef __linux__
      char path[PATH_MAX];
      const ssize_t len = readlink("/proc/self/exe", path, sizeof(path) - 1);
      if (len > 0) {
        path[len] = '\0';
        return std::filesystem::path(path).parent_path().string();
      }
#endif
      return {};
    }

    /**
     * @brief Check whether a candidate directory contains the built Web UI.
     *
     * @param candidate Candidate assets directory.
     * @return True when `web/index.html` exists under the candidate.
     */
    bool has_web_ui(const std::filesystem::path &candidate) {
      std::error_code ec;
      return std::filesystem::exists(candidate / "web" / "index.html", ec);
    }
  }  // namespace

  void init() {
    if (g_initialized) {
      return;
    }
    g_initialized = true;

    const std::string compiled = SUNSHINE_ASSETS_DIR;
    std::vector<std::filesystem::path> candidates;
    candidates.emplace_back(compiled);

    if (const auto exec_dir = executable_dir(); !exec_dir.empty()) {
      const auto base = std::filesystem::path(exec_dir);
      candidates.push_back(base / "assets");
      candidates.push_back(base / ".." / "assets");
    }

    for (const auto &candidate : candidates) {
      std::error_code ec;
      const auto resolved = std::filesystem::weakly_canonical(candidate, ec);
      if (ec || !has_web_ui(resolved)) {
        continue;
      }

      g_root = resolved.string();
      BOOST_LOG(info) << "Using assets directory: "sv << g_root;
      return;
    }

    g_root = compiled;
    BOOST_LOG(warning) << "Assets directory not found, using compiled path: "sv << g_root;
  }

  const std::string &root() {
    if (!g_initialized) {
      init();
    }
    return g_root;
  }

  std::string web() {
    auto path = root();
    if (!path.empty() && path.back() != '/') {
      path += '/';
    }
    return path + "web/";
  }

  std::string join(const std::string &relative) {
    return (std::filesystem::path(root()) / relative).string();
  }
}  // namespace assets_path

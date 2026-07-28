/**
 * @file tests/unit/platform/linux/test_capabilities.cpp
 * @brief Unit tests for Linux capability drop helpers used by portal capture.
 */
#include "../../../tests_common.h"

#if defined(__linux__) && !defined(__FreeBSD__)

  // standard includes
  #include <mutex>

  // platform includes
  #include <sys/capability.h>
  #include <sys/prctl.h>

  // local includes
  #include <src/platform/common.h>
  #include <src/platform/linux/misc.h>

namespace {

  /**
   * @brief Raise a permitted capability into the effective set when available.
   *
   * @param capability Capability value to raise.
   * @return True when the capability is now effective.
   */
  bool try_raise_effective(cap_value_t capability) {
    std::lock_guard lock(platf::capability_mutex());
    cap_t caps = cap_get_proc();
    if (!caps) {
      return false;
    }

    cap_flag_value_t permitted = CAP_CLEAR;
    if (cap_get_flag(caps, capability, CAP_PERMITTED, &permitted) != 0 || permitted != CAP_SET) {
      cap_free(caps);
      return false;
    }

    const bool ok = cap_set_flag(caps, CAP_EFFECTIVE, 1, &capability, CAP_SET) == 0 && cap_set_proc(caps) == 0;
    cap_free(caps);
    return ok;
  }

}  // namespace

TEST(LinuxCapabilitiesTest, CapabilityMutexIsLockable) {
  std::lock_guard lock(platf::capability_mutex());
  SUCCEED();
}

TEST(LinuxCapabilitiesTest, DropElevatedMakesProcessPortalSafe) {
  // Best-effort: when the test binary has file capabilities (or runs privileged),
  // raise them first so drop has something real to clear.
  (void) try_raise_effective(CAP_SYS_ADMIN);
  (void) try_raise_effective(CAP_SYS_NICE);

  platf::drop_elevated_privileges(true);

  EXPECT_FALSE(platf::has_elevated_privileges(true));
  EXPECT_FALSE(platf::has_elevated_privileges(false));
  EXPECT_EQ(1, prctl(PR_GET_DUMPABLE));
}

TEST(LinuxCapabilitiesTest, DropIsIdempotent) {
  platf::drop_elevated_privileges(true);
  platf::drop_elevated_privileges(true);

  EXPECT_FALSE(platf::has_elevated_privileges(true));
  EXPECT_EQ(1, prctl(PR_GET_DUMPABLE));
}

#endif

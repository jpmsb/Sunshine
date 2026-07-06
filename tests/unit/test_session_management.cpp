/**
 * @file tests/unit/test_session_management.cpp
 * @brief Tests for connected client session helpers and APIs.
 */

// test imports
#include "../tests_common.h"

// local imports
#include <src/nvhttp.h>
#include <src/rtsp.h>
#include <src/stream.h>

TEST(SessionManagementTest, FormatSessionLabelWithName) {
  EXPECT_EQ(
    stream::session::format_session_label("192.168.1.10", 47998, "Phone"),
    "192.168.1.10:47998 - Phone"
  );
}

TEST(SessionManagementTest, FormatSessionLabelWithoutName) {
  EXPECT_EQ(
    stream::session::format_session_label("192.168.1.10", 47998, ""),
    "192.168.1.10:47998"
  );
}

TEST(SessionManagementTest, FormatClientNotificationBodyWithName) {
  EXPECT_EQ(
    stream::session::format_client_notification_body("192.168.1.10", 47998, "Phone"),
    "Name: Phone\nIP: 192.168.1.10\nPort: 47998"
  );
}

TEST(SessionManagementTest, FormatClientNotificationBodyWithoutName) {
  EXPECT_EQ(
    stream::session::format_client_notification_body("192.168.1.10", 47998, ""),
    "IP: 192.168.1.10\nPort: 47998"
  );
}

TEST(SessionManagementTest, FormatClientNotificationBodyZeroPort) {
  EXPECT_EQ(
    stream::session::format_client_notification_body("192.168.1.10", 0, ""),
    "IP: 192.168.1.10\nPort: 0"
  );
}

TEST(SessionManagementTest, GetClientMetadataByCertUnknown) {
  const auto metadata = nvhttp::get_client_metadata_by_cert("unknown-cert");
  EXPECT_TRUE(metadata.uuid.empty());
  EXPECT_TRUE(metadata.name.empty());
}

TEST(SessionManagementTest, ListActiveSessionsEmpty) {
  EXPECT_TRUE(rtsp_stream::list_active_sessions().empty());
}

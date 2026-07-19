/**
 * @file tests/unit/test_session_management.cpp
 * @brief Tests for connected client session helpers and APIs.
 */

// test imports
#include "../tests_common.h"

// standard includes
#include <filesystem>

// local imports
#include <src/config.h>
#include <src/nvhttp.h>
#include <src/rtsp.h>
#include <src/stream.h>

namespace {

  /**
   * @brief Test fixture for nvhttp paired-client metadata helpers.
   */
  class NvhttpClientMetadataTest: public BaseTest {
  protected:
    std::string saved_file_state;
    std::filesystem::path temp_state_file;

    void SetUp() override {
      BaseTest::SetUp();
      saved_file_state = config::nvhttp.file_state;
      temp_state_file = std::filesystem::temp_directory_path() / "sunshine_test_nvhttp_state.json";  // NOSONAR(cpp:S5443) - safe for tests
      if (std::filesystem::exists(temp_state_file)) {
        std::filesystem::remove(temp_state_file);
      }
      config::nvhttp.file_state = temp_state_file.string();
      nvhttp::erase_all_clients();
    }

    void TearDown() override {
      nvhttp::erase_all_clients();
      config::nvhttp.file_state = saved_file_state;
      if (std::filesystem::exists(temp_state_file)) {
        std::filesystem::remove(temp_state_file);
      }
      BaseTest::TearDown();
    }
  };

}  // namespace

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

TEST(SessionManagementTest, FormatClientNotificationBodyWithMonitor) {
  EXPECT_EQ(
    stream::session::format_client_notification_body("192.168.1.10", 47998, "Phone", "Dell U2720Q"),
    "Name: Phone\nIP: 192.168.1.10\nPort: 47998\nMonitor: Dell U2720Q"
  );
}

TEST(SessionManagementTest, FormatClientNotificationBodyMonitorLastLineWithoutName) {
  EXPECT_EQ(
    stream::session::format_client_notification_body("192.168.1.10", 47998, "", "HDMI-A-1"),
    "IP: 192.168.1.10\nPort: 47998\nMonitor: HDMI-A-1"
  );
}

TEST(SessionManagementTest, FormatPairingRequestNotificationBody) {
  EXPECT_EQ(
    stream::session::format_pairing_request_notification_body("192.168.1.10", 47984, "Click here to enter the PIN."),
    "IP: 192.168.1.10\nPort: 47984\nClick here to enter the PIN."
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

TEST_F(NvhttpClientMetadataTest, SetClientNameUpdatesClient) {
  constexpr auto test_uuid = "4D7BB2DD-5704-A405-B41C-891A022932E1";
  ASSERT_TRUE(nvhttp::test_insert_client({
    .name = "Phone",
    .uuid = test_uuid,
  }));

  EXPECT_TRUE(nvhttp::set_client_name(test_uuid, "Living Room TV"));

  const auto clients = nvhttp::get_all_clients();
  ASSERT_EQ(clients.size(), 1);
  EXPECT_EQ(clients[0]["name"], "Living Room TV");
}

TEST_F(NvhttpClientMetadataTest, SetClientNameRejectsInvalidValues) {
  constexpr auto test_uuid = "4D7BB2DD-5704-A405-B41C-891A022932E2";
  ASSERT_TRUE(nvhttp::test_insert_client({
    .name = "Phone",
    .uuid = test_uuid,
  }));

  EXPECT_FALSE(nvhttp::set_client_name(test_uuid, ""));
  EXPECT_FALSE(nvhttp::set_client_name(test_uuid, std::string(129, 'a')));
  EXPECT_FALSE(nvhttp::set_client_name("missing-uuid", "Valid Name"));
}

TEST_F(NvhttpClientMetadataTest, TouchClientEndpointUpdatesMetadata) {
  constexpr auto test_uuid = "4D7BB2DD-5704-A405-B41C-891A022932E3";
  ASSERT_TRUE(nvhttp::test_insert_client({
    .name = "Phone",
    .uuid = test_uuid,
  }));

  EXPECT_TRUE(nvhttp::touch_client_endpoint(test_uuid, "192.168.1.20", 47998));

  const auto clients = nvhttp::get_all_clients();
  ASSERT_EQ(clients.size(), 1);
  EXPECT_EQ(clients[0]["last_address"], "192.168.1.20");
  EXPECT_EQ(clients[0]["last_port"], 47998);
}

TEST_F(NvhttpClientMetadataTest, GetAllClientsIncludesMetadataFields) {
  constexpr auto test_uuid = "4D7BB2DD-5704-A405-B41C-891A022932E4";
  ASSERT_TRUE(nvhttp::test_insert_client({
    .name = "Phone",
    .uuid = test_uuid,
    .paired_at = "2026-07-06T12:34:56Z",
    .last_address = "10.0.0.5",
    .last_port = 48010,
  }));

  const auto clients = nvhttp::get_all_clients();
  ASSERT_EQ(clients.size(), 1);
  EXPECT_EQ(clients[0]["paired_at"], "2026-07-06T12:34:56Z");
  EXPECT_EQ(clients[0]["last_address"], "10.0.0.5");
  EXPECT_EQ(clients[0]["last_port"], 48010);
}

TEST_F(NvhttpClientMetadataTest, ClientMetadataPersistsAcrossReload) {
  constexpr auto test_uuid = "4D7BB2DD-5704-A405-B41C-891A022932E5";
  ASSERT_TRUE(nvhttp::test_insert_client({
    .name = "Phone",
    .uuid = test_uuid,
    .paired_at = "2026-07-06T12:34:56Z",
    .last_address = "10.0.0.8",
    .last_port = 48000,
  }));

  ASSERT_TRUE(nvhttp::set_client_name(test_uuid, "Tablet"));
  nvhttp::test_reload_clients_from_disk();

  const auto clients = nvhttp::get_all_clients();
  ASSERT_EQ(clients.size(), 1);
  EXPECT_EQ(clients[0]["name"], "Tablet");
  EXPECT_EQ(clients[0]["paired_at"], "2026-07-06T12:34:56Z");
  EXPECT_EQ(clients[0]["last_address"], "10.0.0.8");
  EXPECT_EQ(clients[0]["last_port"], 48000);
}

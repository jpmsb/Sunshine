/**
 * @file tests/unit/test_portal_options.cpp
 * @brief Unit tests for portal session option helpers.
 */
#include "../tests_common.h"

#include <src/config.h>
#include <src/platform/linux/portal_options.h>

TEST(PortalOptionsTest, SourceTypesForPortalAreMonitorOnly) {
  EXPECT_EQ(portal::SOURCE_TYPE_MONITOR, portal::source_types_for_mode(portal::capture_mode_e::portal));
}

TEST(PortalOptionsTest, SourceTypesForScreencastIncludeMonitorAndWindow) {
  constexpr auto expected = portal::SOURCE_TYPE_MONITOR | portal::SOURCE_TYPE_WINDOW;
  EXPECT_EQ(expected, portal::source_types_for_mode(portal::capture_mode_e::screencast));
}

TEST(PortalOptionsTest, TokenFilenameDiffersByMode) {
  EXPECT_EQ(portal::PORTAL_TOKEN_FILENAME, portal::token_filename_for_mode(portal::capture_mode_e::portal));
  EXPECT_EQ(portal::SCREENCAST_TOKEN_FILENAME, portal::token_filename_for_mode(portal::capture_mode_e::screencast));
}

TEST(PortalOptionsTest, PortalPersistAlwaysEnabled) {
  EXPECT_TRUE(portal::persist_enabled_for_mode(portal::capture_mode_e::portal, false));
  EXPECT_TRUE(portal::persist_enabled_for_mode(portal::capture_mode_e::portal, true));
}

TEST(PortalOptionsTest, ScreencastPersistFollowsConfig) {
  EXPECT_FALSE(portal::persist_enabled_for_mode(portal::capture_mode_e::screencast, false));
  EXPECT_TRUE(portal::persist_enabled_for_mode(portal::capture_mode_e::screencast, true));
}

TEST(PortalOptionsTest, PersistModeAlwaysUntilRevoked) {
  EXPECT_EQ(portal::PERSIST_MODE_UNTIL_REVOKED, portal::persist_mode_for_disk_flag(false));
  EXPECT_EQ(portal::PERSIST_MODE_UNTIL_REVOKED, portal::persist_mode_for_disk_flag(true));
}

TEST(PortalOptionsTest, ScreencastPrefersScreenCastOnlySession) {
  EXPECT_TRUE(portal::prefer_screencast_only_session(portal::capture_mode_e::screencast, false, false));
  EXPECT_TRUE(portal::prefer_screencast_only_session(portal::capture_mode_e::screencast, true, true));
}

TEST(PortalOptionsTest, PortalPrefersScreenCastOnlyWhenPersistingOrRestoring) {
  EXPECT_TRUE(portal::prefer_screencast_only_session(portal::capture_mode_e::portal, true, false));
  EXPECT_TRUE(portal::prefer_screencast_only_session(portal::capture_mode_e::portal, false, true));
  EXPECT_FALSE(portal::prefer_screencast_only_session(portal::capture_mode_e::portal, false, false));
}

TEST(PortalOptionsTest, SessionOptionsForPortal) {
  const auto options = portal::session_options_t::for_mode(portal::capture_mode_e::portal, false);
  EXPECT_EQ(portal::SOURCE_TYPE_MONITOR, options.source_types);
  EXPECT_TRUE(options.persist_enabled);
  EXPECT_EQ(portal::PORTAL_TOKEN_FILENAME, options.token_filename);
}

TEST(PortalOptionsTest, SessionOptionsForScreencastWithoutPersist) {
  const auto options = portal::session_options_t::for_mode(portal::capture_mode_e::screencast, false);
  EXPECT_EQ(portal::SOURCE_TYPE_MONITOR | portal::SOURCE_TYPE_WINDOW, options.source_types);
  EXPECT_FALSE(options.persist_enabled);
  EXPECT_EQ(portal::SCREENCAST_TOKEN_FILENAME, options.token_filename);
  EXPECT_EQ(portal::capture_mode_e::screencast, options.mode);
}

TEST(PortalOptionsTest, SessionOptionsForScreencastWithPersist) {
  const auto options = portal::session_options_t::for_mode(portal::capture_mode_e::screencast, true);
  EXPECT_EQ(portal::SOURCE_TYPE_MONITOR | portal::SOURCE_TYPE_WINDOW, options.source_types);
  EXPECT_TRUE(options.persist_enabled);
  EXPECT_EQ(portal::SCREENCAST_TOKEN_FILENAME, options.token_filename);
  EXPECT_EQ(portal::capture_mode_e::screencast, options.mode);
}

TEST(PortalOptionsTest, ScreencastPersistConfigDefaultIsFalse) {
  EXPECT_FALSE(config::video.screencast_persist);
}

TEST(PortalOptionsTest, ParsePlaceholderColorRgb) {
  const auto color = portal::parse_placeholder_color("#112233");
  ASSERT_TRUE(color.has_value());
  EXPECT_EQ(0x11, color->r);
  EXPECT_EQ(0x22, color->g);
  EXPECT_EQ(0x33, color->b);
  EXPECT_EQ(255, color->a);
}

TEST(PortalOptionsTest, ParsePlaceholderColorRgba) {
  const auto color = portal::parse_placeholder_color("#aabbccdd");
  ASSERT_TRUE(color.has_value());
  EXPECT_EQ(0xaa, color->r);
  EXPECT_EQ(0xbb, color->g);
  EXPECT_EQ(0xcc, color->b);
  EXPECT_EQ(0xdd, color->a);
}

TEST(PortalOptionsTest, ParsePlaceholderColorInvalidFallsBackToBlack) {
  EXPECT_FALSE(portal::parse_placeholder_color("112233").has_value());
  EXPECT_FALSE(portal::parse_placeholder_color("#xyz").has_value());
  const auto color = portal::placeholder_color_or_default("not-a-color");
  EXPECT_EQ(0, color.r);
  EXPECT_EQ(0, color.g);
  EXPECT_EQ(0, color.b);
  EXPECT_EQ(255, color.a);
}

TEST(PortalOptionsTest, ResolvePlaceholderTextUsesHostnameWhenEmpty) {
  EXPECT_EQ("my-host", portal::resolve_placeholder_text("", "my-host"));
  EXPECT_EQ("Hello", portal::resolve_placeholder_text("Hello", "my-host"));
}

TEST(PortalOptionsTest, ScreencastPlaceholderNameIsStable) {
  EXPECT_EQ("screencast-placeholder", portal::SCREENCAST_PLACEHOLDER_NAME);
}

TEST(PortalOptionsTest, PlaceholderConfigDefaults) {
  EXPECT_EQ("#000000", config::video.screencast_placeholder_color);
  EXPECT_TRUE(config::video.screencast_placeholder_text.empty());
}

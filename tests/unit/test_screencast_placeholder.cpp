/**
 * @file tests/unit/test_screencast_placeholder.cpp
 * @brief Unit tests for screencast placeholder font bit order and encode device wiring.
 */
#include "../tests_common.h"

#if defined(SUNSHINE_BUILD_PORTAL)

  #include <src/platform/linux/portal_options.h>
  #include <src/video.h>

  namespace {

    /**
     * @brief Minimal stream config used when constructing the placeholder display.
     * @return Populated video config with a small 8-bit SDR profile.
     */
    video::config_t placeholder_test_config() {
      video::config_t config {};
      config.width = 1280;
      config.height = 720;
      config.framerate = 60;
      config.framerateX100 = 6000;
      config.bitrate = 10000;
      config.slicesPerFrame = 1;
      config.numRefFrames = 1;
      config.encoderCscMode = 0;
      config.videoFormat = 0;
      config.dynamicRange = 0;
      config.chromaSamplingType = 0;
      config.enableIntraRefresh = 0;
      return config;
    }

  }  // namespace

  /**
   * @brief Glyph A row 0 is 0x0C with LSB-left bits: columns 2 and 3 set.
   */
  TEST(ScreencastPlaceholderTest, GlyphATopRowUsesLsbLeftBitOrder) {
    // 0x0C == 0b00001100 → cols 2 and 3 on when LSB is leftmost.
    EXPECT_FALSE(portal::placeholder_font_bit('A', 0, 0));
    EXPECT_FALSE(portal::placeholder_font_bit('A', 0, 1));
    EXPECT_TRUE(portal::placeholder_font_bit('A', 0, 2));
    EXPECT_TRUE(portal::placeholder_font_bit('A', 0, 3));
    EXPECT_FALSE(portal::placeholder_font_bit('A', 0, 4));
    EXPECT_FALSE(portal::placeholder_font_bit('A', 0, 5));
    EXPECT_FALSE(portal::placeholder_font_bit('A', 0, 6));
    EXPECT_FALSE(portal::placeholder_font_bit('A', 0, 7));
  }

  /**
   * @brief System memory probe keeps the empty software encode device.
   */
  TEST(ScreencastPlaceholderTest, SystemMemTypeReturnsSoftwareEncodeDevice) {
    auto display = portal::make_screencast_placeholder_display(platf::mem_type_e::system, placeholder_test_config());
    ASSERT_NE(display, nullptr);

    auto device = display->make_avcodec_encode_device(platf::pix_fmt_e::yuv420p);
    ASSERT_NE(device, nullptr);
    EXPECT_EQ(device->data, nullptr);
  }

  #if defined(SUNSHINE_BUILD_VAAPI)
  /**
   * @brief VAAPI probe must not return an empty stub (forces FFmpeg av_hwdevice_ctx_create).
   *
   * When the local DRM render node is unavailable the factory returns nullptr; that is
   * still preferable to a non-null device with a null `data` callback.
   */
  TEST(ScreencastPlaceholderTest, VaapiMemTypeWiresSunshineInitCallback) {
    auto display = portal::make_screencast_placeholder_display(platf::mem_type_e::vaapi, placeholder_test_config());
    ASSERT_NE(display, nullptr);

    auto device = display->make_avcodec_encode_device(platf::pix_fmt_e::yuv420p);
    if (!device) {
      GTEST_SKIP() << "VAAPI encode device unavailable (no usable DRM render node)";
    }
    EXPECT_NE(device->data, nullptr);
  }
  #endif  // SUNSHINE_BUILD_VAAPI

#endif  // SUNSHINE_BUILD_PORTAL

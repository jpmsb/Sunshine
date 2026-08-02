/**
 * @file tests/unit/test_screencast_placeholder.cpp
 * @brief Unit tests for screencast placeholder font bit order.
 */
#include "../tests_common.h"

#if defined(SUNSHINE_BUILD_PORTAL)

  #include <src/platform/linux/portal_options.h>

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

#endif  // SUNSHINE_BUILD_PORTAL

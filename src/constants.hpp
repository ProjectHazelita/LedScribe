#pragma once

#include <array>
#include <cstdint>

// ── display / protocol ──
constexpr int ROWS = 11;            // display height in pixels
constexpr int DISPLAY_WIDTH = 44;   // visible width of one animation frame
constexpr int MAX_PAYLOAD = 8192;   // CH546 safe limit (bytes)
constexpr int REPORT_SIZE = 64;     // HID report size (bytes)
constexpr int COLUMN_WIDTH = 8;     // bits per packed column group
constexpr int BRIGHTNESS_DIV = 127; // vendor / pack_image binary threshold
constexpr int STB_THRESHOLD = 96;   // stb_truetype grayscale → binary threshold
constexpr int BRIGHT = 255;
constexpr int DARK = 0;
constexpr int MAX_MESSAGES = 8;

// ── vendor font metrics ──
constexpr int ASCII_GLYPH_WIDTH = 8;                     // ASC11 glyph width
constexpr int GBK_GLYPH_WIDTH = 11;                      // HZK11 glyph width (used)
constexpr int GBK_STORAGE_WIDTH = 16;                    // HZK11 glyph width (stored)
constexpr int GBK_BYTES_PER_ROW = 2;                     // HZK11: 16-bit rows → 2 bytes
constexpr int GBK_GLYPH_SIZE = ROWS * GBK_BYTES_PER_ROW; // 22 bytes per GBK glyph
constexpr int ASCII_FONT_SIZE = 256 * ROWS;

// ── GBK codec ──
constexpr int GBK_LEAD_MIN = 0x81;   // first lead byte in HZK11
constexpr int GBK_TRAIL_MIN = 0x40;  // first trail byte in HZK11
constexpr int GBK_TRAIL_COUNT = 191; // trail slots per lead byte
constexpr int GBK_SLOT_COUNT = 126 * GBK_TRAIL_COUNT;
constexpr int GBK_FONT_SIZE = GBK_SLOT_COUNT * GBK_GLYPH_SIZE;

constexpr std::array<uint8_t, 64> HEADER_TEMPLATE = {
    0x77, 0x61, 0x6E, 0x67, 0x00, 0x00, 0x00, 0x00, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

// UTF-8 codec (RFC 3629)
// Layout: leading byte → codepoint data bits, trailer bytes each carry 6 bits
namespace utf8
{
constexpr unsigned char ASCII_LIMIT = 0x80; // 0xxxxxxx → single-byte ASCII
constexpr unsigned char TWO_MASK = 0xE0;    // 110xxxxx → 2-byte sequence
constexpr unsigned char TWO_MATCH = 0xC0;
constexpr unsigned char TWO_BITS = 0x1F;   // codepoint bits in lead byte
constexpr unsigned char THREE_MASK = 0xF0; // 1110xxxx → 3-byte (CJK)
constexpr unsigned char THREE_MATCH = 0xE0;
constexpr unsigned char THREE_BITS = 0x0F;
constexpr unsigned char FOUR_MASK = 0xF8; // 11110xxx → 4-byte (emoji)
constexpr unsigned char FOUR_MATCH = 0xF0;
constexpr unsigned char FOUR_BITS = 0x07;
constexpr int SHIFT = 6;                   // each trailer contributes 6 bits
constexpr unsigned char TRAIL_BITS = 0x3F; // 10xxxxxx → extract 6 data bits
} // namespace utf8

// ── USB ──
constexpr uint16_t VID = 0x0416;
constexpr uint16_t PID = 0x5020;
constexpr unsigned int TRANSFER_TIMEOUT = 3000;

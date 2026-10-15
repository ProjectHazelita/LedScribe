#include <array>
#include <cstdint>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <vector>

#include "constants.hpp"
#include "pack.hpp"
#include "protocol.hpp"
#include "utils.hpp"
#include "vendor_font.hpp"

namespace
{
auto require(bool condition, std::string_view message) -> void
{
    if (!condition)
    {
        throw std::runtime_error(std::string(message));
    }
}

template <typename Exception, typename Function>
auto require_throws(Function &&function, std::string_view message) -> void
{
    try
    {
        std::invoke(std::forward<Function>(function));
    }
    catch (const Exception &)
    {
        return;
    }
    throw std::runtime_error(std::string(message));
}

auto test_pack_image() -> void
{
    std::vector<uint8_t> pixels(static_cast<std::size_t>(ROWS * 8), DARK);
    pixels[0] = BRIGHT;
    pixels.back() = BRIGHT;
    const auto packed = pack_image(pixels, 8);
    require(packed.size() == ROWS, "8-pixel wide bitmap should encode to 11 bytes");
    require(packed.front() == 0x80, "top-left pixel should be the most significant bit");
    require(packed.back() == 0x01, "bottom-right pixel should be the least significant bit");

    require_throws<std::invalid_argument>([]() -> void { pack_image({}, 0); },
                                          "width=0 should be rejected");
    require_throws<std::invalid_argument>([]() -> void
                                          { pack_image(std::array<uint8_t, 23>{}, 2); },
                                          "excess pixel data should be rejected");
}

auto test_header_and_payload() -> void
{
    DisplaySettings settings;
    settings.slots[0].speed = 4;
    settings.slots[0].mode = DisplayMode::up;
    settings.slots[0].blink = true;
    settings.slots[0].border = true;
    settings.brightness = 50;
    const ProtocolTimestamp timestamp{
        .year = 2026, .month = 7, .day = 17, .hour = 19, .minute = 0, .second = 1};
    const std::array<uint16_t, 1> lengths{3};
    const auto header = build_header(lengths, settings, timestamp);
    require(std::string_view(reinterpret_cast<const char *>(header.data()), 4) == "wang",
            "header magic error");
    require(header[5] == 0x20 && header[6] == 1 && header[7] == 1, "header flag error");
    require(header[8] == 0x32 && header[16] == 0 && header[17] == 3, "header settings error");
    require(header[38] == 26 && header[43] == 1, "header timestamp error");

    GlyphBitmap image{.pixels = std::vector<uint8_t>(static_cast<std::size_t>(ROWS * 9), DARK),
                      .width = 9};
    const std::array images{image};
    const auto payload = build_payload(images, {}, timestamp);
    require(payload.size() % REPORT_SIZE == 0, "payload not padded to 64 bytes");
    require(payload.size() == 128, "9-pixel bitmap payload should be 128 bytes");
}

auto test_mode_encoding() -> void
{
    // Protocol mapping predates the CLI translation; declaration order must not change it.
    constexpr std::array modes{
        DisplayMode::left, DisplayMode::right, DisplayMode::up,
        DisplayMode::down, DisplayMode::still, DisplayMode::animation,
        DisplayMode::drop, DisplayMode::curtain, DisplayMode::laser};
    const std::array<uint16_t, 1> lengths{1};
    const ProtocolTimestamp timestamp{
        .year = 2026, .month = 7, .day = 17, .hour = 19, .minute = 0, .second = 1};
    for (std::size_t code = 0; code < modes.size(); ++code)
    {
        DisplaySettings settings;
        settings.slots[0].speed = 8;
        settings.slots[0].mode = modes[code];
        const auto header = build_header(lengths, settings, timestamp);
        require(header[8] == 0x70 + code, "named mode serialized with incorrect protocol code");
    }
}

auto test_utf8_validation() -> void
{
    const auto codepoints = utf8_codepoints("A洛🙂");
    require(codepoints.size() == 3, "UTF-8 decode count error");
    require_throws<EncodingError>([]() -> void { utf8_codepoints("\xE6\xB4"); },
                                  "truncated UTF-8 should be rejected");
    require_throws<EncodingError>([]() -> void { utf8_codepoints("\xC0\x80"); },
                                  "overlong UTF-8 should be rejected");
}

auto test_vendor_font() -> void
{
    VendorFont font{.ascii = std::vector<uint8_t>(ASCII_FONT_SIZE),
                    .gbk = std::vector<uint8_t>(GBK_FONT_SIZE)};
    font.ascii[static_cast<std::size_t>('A') * ROWS] = 0x81;
    const auto ascii = render_vendor("A", font);
    require(ascii.width == 8 && ascii.pixels[0] == BRIGHT && ascii.pixels[7] == BRIGHT,
            "ASCII glyph decode error");

    const auto encoded = utf8_to_gbk("洛");
    const auto index = (static_cast<std::size_t>(encoded[0] - GBK_LEAD_MIN) *
                        static_cast<std::size_t>(GBK_TRAIL_COUNT)) +
                       static_cast<std::size_t>(encoded[1] - GBK_TRAIL_MIN);
    const auto offset = index * static_cast<std::size_t>(GBK_GLYPH_SIZE);
    font.gbk[offset] = 0x80;
    font.gbk[offset + 1] = 0x20;
    const auto gbk = render_vendor("洛", font);
    require(gbk.width == 11 && gbk.pixels[0] == BRIGHT && gbk.pixels[10] == BRIGHT,
            "GBK glyph decode error");
    require(render_vendor("A洛", font).width == 19, "mixed glyph width error");
    require(render_vendor("拓展天空", font).width == 44,
            "four GBK characters should fill 44 columns");
}
} // namespace

auto main() -> int
{
    try
    {
        test_pack_image();
        test_header_and_payload();
        test_mode_encoding();
        test_utf8_validation();
        test_vendor_font();
        std::cout << "all tests passed\n";
        return 0;
    }
    catch (const std::exception &error)
    {
        std::cerr << "test failed: " << error.what() << '\n';
        return 1;
    }
}

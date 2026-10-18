#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
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
    const auto packed = pack_image(pixels, 8, 8);
    require(packed.size() == ROWS, "8-pixel wide bitmap should encode to 11 bytes");
    require(packed.front() == 0x80, "top-left pixel should be the most significant bit");
    require(packed.back() == 0x01, "bottom-right pixel should be the least significant bit");

    require_throws<std::invalid_argument>([]() -> void { pack_image({}, 0, 8); },
                                          "width=0 should be rejected");
    require_throws<std::invalid_argument>([]() -> void
                                          { pack_image(std::array<uint8_t, 23>{}, 2, 2); },
                                          "excess pixel data should be rejected");
    require_throws<std::invalid_argument>(
        []() -> void { pack_image(std::array<uint8_t, static_cast<long>(ROWS) * 8>{}, 8, 0); },
        "frame width=0 should be rejected");
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
    constexpr std::array<std::string_view, 9> names{
        "left", "right", "up", "down", "still", "animation", "drop", "curtain", "laser"};
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
        require(display_mode_name(modes[code]) == names[code],
                "canonical mode name must match protocol declaration order");
        require(parse_display_mode(names[code]) == modes[code],
                "canonical mode name must parse back to its protocol code");
        const auto numeric = std::to_string(code);
        require(parse_display_mode(numeric) == modes[code],
                "numeric mode must parse to its protocol code");
    }
    require_throws<std::invalid_argument>([]() -> void { parse_display_mode("9"); },
                                          "numeric mode above 8 must be rejected");
    require_throws<std::invalid_argument>([]() -> void { parse_display_mode("-1"); },
                                          "negative numeric mode must be rejected");
    require_throws<std::invalid_argument>([]() -> void { parse_display_mode("Animation"); },
                                          "mode names are case-sensitive");
    require_throws<std::invalid_argument>([]() -> void { parse_display_mode(""); },
                                          "empty mode must be rejected");
    require_throws<std::invalid_argument>([]() -> void { parse_display_mode("left "); },
                                          "mode with trailing text must be rejected");
    require_throws<std::invalid_argument>(
        []() -> void { static_cast<void>(display_mode_name(static_cast<DisplayMode>(9))); },
        "out-of-range display mode must not get a canonical name");
}

auto test_animation_frame_alignment() -> void
{
    const ProtocolTimestamp timestamp{
        .year = 2026, .month = 7, .day = 17, .hour = 19, .minute = 0, .second = 1};
    for (const int width : {1, 40, 44, 45, 88, 176})
    {
        const auto source_width = static_cast<std::size_t>(width);
        GlyphBitmap image{
            .pixels = std::vector<uint8_t>(source_width * static_cast<std::size_t>(ROWS), DARK),
            .width = width};
        for (std::size_t x = 0; x < source_width; ++x)
        {
            // Distinct marks at frame edges and in each frame's interior expose drift.
            image.pixels[(x % static_cast<std::size_t>(ROWS)) * source_width + x] = BRIGHT;
            if (x % 44 == 0 || x % 44 == 43) image.pixels[x] = BRIGHT;
        }
        GlyphBitmap following{
            .pixels = std::vector<uint8_t>(static_cast<std::size_t>(ROWS) * 8, DARK),
            .width = 8};
        following.pixels.front() = BRIGHT;
        following.pixels.back() = BRIGHT;
        const std::array images{image, following};
        for (const auto mode : {DisplayMode::animation, DisplayMode::left})
        {
            DisplaySettings settings;
            settings.slots[0].mode = mode;
            const auto payload = build_payload(images, settings, timestamp);
            const std::size_t frame_width = mode == DisplayMode::animation ? 44 : source_width;
            const auto frames = (source_width + frame_width - 1) / frame_width;
            const auto frame_groups = (frame_width + 7) / 8;
            const auto groups = frames * frame_groups;
            require(((static_cast<std::size_t>(payload[16]) << 8U) | payload[17]) == groups,
                    "message length must include padding within every animation frame");
            require(payload[18] == 0 && payload[19] == 1,
                    "following message length must remain independent of animation framing");
            for (std::size_t frame = 0; frame < frames; ++frame)
            {
                for (std::size_t row = 0; row < static_cast<std::size_t>(ROWS); ++row)
                {
                    for (std::size_t x = 0; x < frame_groups * 8; ++x)
                    {
                        const auto offset = REPORT_SIZE +
                            ((frame * frame_groups + x / 8) * static_cast<std::size_t>(ROWS)) + row;
                        const bool lit = (payload[offset] & (0x80U >> (x % 8))) != 0;
                        const auto source_x = frame * frame_width + x;
                        const bool expected = x < frame_width && source_x < source_width &&
                            image.pixels[row * source_width + source_x] == BRIGHT;
                        require(lit == expected,
                                "frame pixels must retain their positions with dark padding");
                    }
                }
            }
            const auto next_offset = REPORT_SIZE + groups * static_cast<std::size_t>(ROWS);
            for (std::size_t row = 0; row < static_cast<std::size_t>(ROWS); ++row)
            {
                const uint8_t expected = row == 0 ? 0x80 : (row == ROWS - 1 ? 0x01 : 0);
                require(payload[next_offset + row] == expected,
                        "animation padding must not consume pixels from the following slot");
            }
        }
    }
}

auto test_animation_capacity() -> void
{
    DisplaySettings settings;
    settings.slots[0].mode = DisplayMode::animation;
    const ProtocolTimestamp timestamp{
        .year = 2026, .month = 7, .day = 17, .hour = 19, .minute = 0, .second = 1};
    // 123 frames occupy 738 groups; header plus report padding fills exactly 8192 bytes.
    GlyphBitmap image{
        .pixels = std::vector<uint8_t>(static_cast<std::size_t>(44U) * 123U * static_cast<std::size_t>(ROWS), DARK),
        .width = 44 * 123};
    const std::array images{image};
    const auto layout = measure_payload(images, settings);
    require(layout.slot_count == 1 && layout.slots[0].frames == 123 &&
                layout.slots[0].groups == 738 &&
                layout.slots[0].bytes == static_cast<std::size_t>(738 * ROWS),
            "123 animation frames must report per-frame padding in the measurement");
    require(layout.raw_bytes == static_cast<std::size_t>(REPORT_SIZE + (738 * ROWS)),
            "raw payload measurement must include the 64-byte header");
    require(layout.payload_bytes == MAX_PAYLOAD && layout.fits,
            "123 animation frames must measure as the largest fitting animation");
    const auto payload = build_payload(images, settings, timestamp);
    require(payload.size() == layout.payload_bytes && payload[16] == 0x02 && payload[17] == 0xE2,
            "largest fitting animation must match its measurement and frame padding");
    image.width += 44;
    image.pixels.resize(static_cast<std::size_t>(image.width) * static_cast<std::size_t>(ROWS));
    const auto oversized = measure_payload(std::array{image}, settings);
    require(oversized.slots[0].frames == 124 && oversized.slots[0].groups == 744 &&
                oversized.slots[0].bytes == static_cast<std::size_t>(744 * ROWS),
            "124 animation frames must report their real padded size");
    require(!oversized.fits && oversized.payload_bytes == 8256,
            "124 animation frames must measure as 8256 bytes and over capacity");
    bool rejected = false;
    try
    {
        static_cast<void>(build_payload(std::array{image}, settings, timestamp));
    }
    catch (const std::invalid_argument &error)
    {
        rejected = true;
        require(std::string_view(error.what()).find("8256") != std::string_view::npos,
                "over-capacity error must report the measured byte count");
    }
    require(rejected, "animation exceeding capacity after frame padding must be rejected");

    // The same 5456-column bitmap is affordable as a continuous strip.
    settings.slots[0].mode = DisplayMode::left;
    const auto continuous = measure_payload(std::array{image}, settings);
    require(continuous.slots[0].frames == 1 && continuous.slots[0].groups == 682 &&
                continuous.slots[0].bytes == static_cast<std::size_t>(682 * ROWS),
            "left mode must re-measure the full strip without animation framing");
    require(continuous.payload_bytes == 7616 && continuous.fits,
            "5456 continuous columns must measure within capacity");
    require(build_payload(std::array{image}, settings, timestamp).size() ==
                continuous.payload_bytes,
            "continuous payload size must match its re-measurement");
}

auto test_payload_measurement() -> void
{
    DisplaySettings settings;
    settings.slots[0].mode = DisplayMode::animation;
    settings.slots[1].mode = DisplayMode::left;
    const ProtocolTimestamp timestamp{
        .year = 2026, .month = 7, .day = 17, .hour = 19, .minute = 0, .second = 1};

    GlyphBitmap animated{
        .pixels = std::vector<uint8_t>(static_cast<std::size_t>(45) * static_cast<std::size_t>(ROWS),
                                       DARK),
        .width = 45};
    GlyphBitmap continuous{
        .pixels = std::vector<uint8_t>(static_cast<std::size_t>(8) * static_cast<std::size_t>(ROWS),
                                       DARK),
        .width = 8};
    const std::array images{animated, continuous};
    auto layout = measure_payload(images, settings);
    require(layout.slot_count == 2, "measurement must count every supplied slot");
    require(layout.slots[0].frames == 2 && layout.slots[0].groups == 12 &&
                layout.slots[0].bytes == static_cast<std::size_t>(12 * ROWS),
            "45-column animation must pad two 44-column frames to six groups each");
    require(layout.slots[1].frames == 1 && layout.slots[1].groups == 1 &&
                layout.slots[1].bytes == static_cast<std::size_t>(ROWS),
            "continuous 8-column bitmap must occupy a single group");
    require(layout.slots[2].frames == 0 && layout.slots[2].groups == 0 &&
                layout.slots[2].bytes == 0,
            "unused slots must remain zeroed in the measurement");
    require(layout.raw_bytes == static_cast<std::size_t>(REPORT_SIZE + (13 * ROWS)),
            "multi-slot raw measurement must sum every slot");
    require(layout.payload_bytes == 256 && layout.fits,
            "multi-slot measurement must round up to a 64-byte report boundary");
    require(build_payload(images, settings, timestamp).size() == layout.payload_bytes,
            "measurement must agree with the encoded multi-slot payload size");

    // A continuous bitmap is re-measured without animation framing when its mode changes.
    settings.slots[0].mode = DisplayMode::left;
    layout = measure_payload(images, settings);
    require(layout.slots[0].frames == 1 && layout.slots[0].groups == 6 &&
                layout.slots[0].bytes == static_cast<std::size_t>(6 * ROWS),
            "left mode must measure 45 continuous columns as six groups");
    require(layout.raw_bytes == static_cast<std::size_t>(REPORT_SIZE + (7 * ROWS)),
            "mode change must recompute raw payload size");
    require(layout.payload_bytes == 192 && layout.fits,
            "continuous 45-column bitmap must round up to 192 bytes");
    require(build_payload(images, settings, timestamp).size() == layout.payload_bytes,
            "measurement must agree with the continuous encoded payload size");

    require_throws<std::invalid_argument>(
        []() -> void { measure_payload(std::span<const GlyphBitmap>{}, DisplaySettings{}); },
        "measurement must reject a missing message");
    const auto valid = GlyphBitmap{
        .pixels = std::vector<uint8_t>(static_cast<std::size_t>(ROWS), DARK), .width = 1};
    std::vector<GlyphBitmap> nine(9, valid);
    require_throws<std::invalid_argument>(
        [&]() -> void { measure_payload(nine, DisplaySettings{}); },
        "measurement must reject more than eight messages");
    const auto malformed = GlyphBitmap{.pixels = {}, .width = 8};
    require_throws<std::invalid_argument>(
        [&]() -> void { measure_payload(std::array{malformed}, DisplaySettings{}); },
        "measurement must validate bitmap dimensions");
    DisplaySettings bad_speed;
    bad_speed.slots[0].speed = 0;
    require_throws<std::invalid_argument>(
        [&]() -> void { measure_payload(std::array{valid}, bad_speed); },
        "measurement must validate slot settings");
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
        test_animation_frame_alignment();
        test_animation_capacity();
        test_payload_measurement();
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

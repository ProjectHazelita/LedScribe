#include "protocol.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <ctime>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <system_error>

#include "pack.hpp"

namespace
{
struct NamedDisplayMode
{
    std::string_view name;
    DisplayMode mode;
};

// Declared order is the wire protocol mapping; do not reorder without changing hardware.
constexpr std::array<NamedDisplayMode, 9> DISPLAY_MODES = {
    NamedDisplayMode{.name = "left", .mode = DisplayMode::left},
    NamedDisplayMode{.name = "right", .mode = DisplayMode::right},
    NamedDisplayMode{.name = "up", .mode = DisplayMode::up},
    NamedDisplayMode{.name = "down", .mode = DisplayMode::down},
    NamedDisplayMode{.name = "still", .mode = DisplayMode::still},
    NamedDisplayMode{.name = "animation", .mode = DisplayMode::animation},
    NamedDisplayMode{.name = "drop", .mode = DisplayMode::drop},
    NamedDisplayMode{.name = "curtain", .mode = DisplayMode::curtain},
    NamedDisplayMode{.name = "laser", .mode = DisplayMode::laser},
};

auto validate_settings(const DisplaySettings &settings) -> void
{
    for (std::size_t i = 0; i < settings.slots.size(); ++i)
    {
        const auto &slot = settings.slots[i];
        if (slot.speed < 1 || slot.speed > 8)
        {
            throw std::invalid_argument("speed of slot " + std::to_string(i) +
                                        " must be between 1..8");
        }
        const int mode = static_cast<int>(slot.mode);
        if (mode < 0 || mode > 8)
        {
            throw std::invalid_argument("speed of slot " + std::to_string(i) +
                                        " must be between 0..8");
        }
    }
    if (settings.brightness != 25 && settings.brightness != 50 && settings.brightness != 75 &&
        settings.brightness != 100)
    {
        throw std::invalid_argument("brightness must be 25, 50, 75 or 100");
    }
}

auto brightness_code(int brightness) -> uint8_t
{
    switch (brightness)
    {
    case 25:
        return 0x40;
    case 50:
        return 0x20;
    case 75:
        return 0x10;
    case 100:
        return 0x00;
    default:
        throw std::invalid_argument("invalid brightness");
    }
}
} // namespace

auto current_timestamp() -> ProtocolTimestamp
{
    const std::time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm local{};
    if (localtime_r(&now, &local) == nullptr)
    {
        throw std::runtime_error("failed to get local time");
    }
    return {.year = local.tm_year + 1900,
            .month = local.tm_mon + 1,
            .day = local.tm_mday,
            .hour = local.tm_hour,
            .minute = local.tm_min,
            .second = local.tm_sec};
}

auto display_mode_name(DisplayMode mode) -> std::string_view
{
    for (const auto &named : DISPLAY_MODES)
    {
        if (named.mode == mode)
        {
            return named.name;
        }
    }
    throw std::invalid_argument("invalid display mode");
}

auto parse_display_mode(std::string_view value) -> DisplayMode
{
    for (const auto &named : DISPLAY_MODES)
    {
        if (value == named.name)
        {
            return named.mode;
        }
    }
    int numeric = 0;
    const auto [end, error] =
        std::from_chars(value.data(), value.data() + value.size(), numeric);
    if (error != std::errc{} || end != value.data() + value.size() || numeric < 0 ||
        numeric > 8)
    {
        throw std::invalid_argument(
            "mode must be left, right, up, down, still, animation, drop, curtain, laser or 0..8");
    }
    return static_cast<DisplayMode>(numeric);
}

auto build_header(std::span<const uint16_t> lengths, const DisplaySettings &settings,
                  const ProtocolTimestamp &timestamp) -> std::array<uint8_t, REPORT_SIZE>
{
    if (lengths.empty() || lengths.size() > MAX_MESSAGES)
    {
        throw std::invalid_argument("must provide 1..8 messages");
    }
    if (std::ranges::any_of(lengths, [](uint16_t length) -> bool { return length == 0; }))
    {
        throw std::invalid_argument("message bitmap length exceeds protocol limit");
    }
    validate_settings(settings);
    if (timestamp.month < 1 || timestamp.month > 12 || timestamp.day < 1 || timestamp.day > 31 ||
        timestamp.hour < 0 || timestamp.hour > 23 || timestamp.minute < 0 ||
        timestamp.minute > 59 || timestamp.second < 0 || timestamp.second > 59)
    {
        throw std::invalid_argument("protocol timestamp out of valid range");
    }

    auto header = HEADER_TEMPLATE;
    header[5] = brightness_code(settings.brightness);

    header[6] = 0;
    header[7] = 0;
    for (std::size_t i = 0; i < lengths.size(); ++i)
    {
        if (settings.slots[i].blink) header[6] |= static_cast<uint8_t>(1U << i);
        if (settings.slots[i].border) header[7] |= static_cast<uint8_t>(1U << i);
    }

    for (std::size_t i = 0; i < MAX_MESSAGES; ++i)
    {
        const auto &slot = settings.slots[i];
        header[8 + i] = static_cast<uint8_t>((16 * (slot.speed - 1)) + static_cast<int>(slot.mode));
    }
    for (std::size_t index = 0; index < lengths.size(); ++index)
    {
        header[16 + (2 * index)] = static_cast<uint8_t>(lengths[index] >> 8U);
        header[17 + (2 * index)] = static_cast<uint8_t>(lengths[index] & 0xFFU);
    }

    header[38] = static_cast<uint8_t>(timestamp.year % 100);
    header[39] = static_cast<uint8_t>(timestamp.month);
    header[40] = static_cast<uint8_t>(timestamp.day);
    header[41] = static_cast<uint8_t>(timestamp.hour);
    header[42] = static_cast<uint8_t>(timestamp.minute);
    header[43] = static_cast<uint8_t>(timestamp.second);
    return header;
}

auto measure_payload(std::span<const GlyphBitmap> images, const DisplaySettings &settings)
    -> PayloadLayout
{
    if (images.empty() || images.size() > MAX_MESSAGES)
    {
        throw std::invalid_argument("must provide 1..8 messages");
    }
    validate_settings(settings);

    PayloadLayout layout{};
    layout.slot_count = images.size();
    layout.raw_bytes = static_cast<std::size_t>(REPORT_SIZE);
    for (std::size_t index = 0; index < images.size(); ++index)
    {
        const auto &image = images[index];
        image.validate();
        const auto source_width = static_cast<std::size_t>(image.width);
        const auto frame_width = settings.slots[index].mode == DisplayMode::animation
                                     ? static_cast<std::size_t>(DISPLAY_WIDTH)
                                     : source_width;
        const auto frames = (source_width + frame_width - 1) / frame_width;
        const auto frame_groups =
            (frame_width + static_cast<std::size_t>(COLUMN_WIDTH) - 1) /
            static_cast<std::size_t>(COLUMN_WIDTH);
        const auto groups = frames * frame_groups;
        if (groups == 0 || groups > std::numeric_limits<uint16_t>::max())
        {
            throw std::invalid_argument("message bitmap length exceeds protocol limit");
        }
        const auto bytes = groups * static_cast<std::size_t>(ROWS);
        layout.slots[index] = {.frames = frames, .groups = groups, .bytes = bytes};
        layout.raw_bytes += bytes;
    }
    layout.payload_bytes =
        ((layout.raw_bytes + REPORT_SIZE - 1) / REPORT_SIZE) * REPORT_SIZE;
    layout.fits = layout.payload_bytes <= MAX_PAYLOAD;
    return layout;
}

auto build_payload(std::span<const GlyphBitmap> images, const DisplaySettings &settings,
                   const ProtocolTimestamp &timestamp) -> std::vector<uint8_t>
{
    const auto layout = measure_payload(images, settings);
    if (!layout.fits)
    {
        throw std::invalid_argument("encoded payload is " + std::to_string(layout.payload_bytes) +
                                    " bytes, exceeding device safety limit of " +
                                    std::to_string(MAX_PAYLOAD) + " bytes");
    }

    std::vector<uint16_t> lengths;
    lengths.reserve(layout.slot_count);
    for (std::size_t index = 0; index < layout.slot_count; ++index)
    {
        lengths.push_back(static_cast<uint16_t>(layout.slots[index].groups));
    }

    const auto header = build_header(lengths, settings, timestamp);
    std::vector<uint8_t> payload;
    payload.reserve(layout.payload_bytes);
    payload.insert(payload.end(), header.begin(), header.end());
    for (std::size_t index = 0; index < images.size(); ++index)
    {
        const auto &image = images[index];
        const int frame_width = settings.slots[index].mode == DisplayMode::animation
                                    ? DISPLAY_WIDTH
                                    : image.width;
        auto packed = pack_image(image.pixels, image.width, frame_width);
        payload.insert(payload.end(), packed.begin(), packed.end());
    }
    payload.resize(layout.payload_bytes, 0);
    return payload;
}

auto preview(const GlyphBitmap &image) -> std::string
{
    image.validate();
    std::string output;
    const auto width = static_cast<std::size_t>(image.width);
    output.reserve(((width * 6) + 1) * static_cast<std::size_t>(ROWS));
    for (int row = 0; row < ROWS; ++row)
    {
        for (int column = 0; column < image.width; ++column)
        {
            const auto index =
                (static_cast<std::size_t>(row) * width) + static_cast<std::size_t>(column);
            output += image.pixels[index] > BRIGHTNESS_DIV ? "██" : "  ";
        }
        if (row + 1 < ROWS)
        {
            output.push_back('\n');
        }
    }
    return output;
}

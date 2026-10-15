#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "constants.hpp"
#include "types.hpp"

enum class DisplayMode : uint8_t
{
    left = 0,      // 左移
    right = 1,     // 右移
    up = 2,        // 上移
    down = 3,      // 下移
    still = 4,     // 固定
    animation = 5, // 动画
    drop = 6,      // 飘雪
    curtain = 7,   // 画卷
    laser = 8,     // 镭射
};

struct SlotSettings
{
    int speed{4};
    DisplayMode mode{DisplayMode::left};
    bool blink{false};
    bool border{false};
};

struct DisplaySettings
{
    std::array<SlotSettings, MAX_MESSAGES> slots{};
    int brightness{50};
};

struct ProtocolTimestamp
{
    int year;
    int month;
    int day;
    int hour;
    int minute;
    int second;
};

auto current_timestamp() -> ProtocolTimestamp;
auto build_header(std::span<const uint16_t> lengths, const DisplaySettings &settings,
                  const ProtocolTimestamp &timestamp) -> std::array<uint8_t, REPORT_SIZE>;
auto build_payload(std::span<const GlyphBitmap> images, const DisplaySettings &settings,
                   const ProtocolTimestamp &timestamp = current_timestamp())
    -> std::vector<uint8_t>;
auto preview(const GlyphBitmap &image) -> std::string;

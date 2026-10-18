#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

#include "truetype.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <vector>

#include "utils.hpp"

namespace
{
constexpr int MIN_FONT_SIZE = 5;
constexpr int HORIZONTAL_PADDING = 1;

struct BitmapDeleter
{
    auto operator()(unsigned char *bitmap) const noexcept -> void
    {
        stbtt_FreeBitmap(bitmap, nullptr);
    }
};

using StbBitmap = std::unique_ptr<unsigned char, BitmapDeleter>;

struct TextMetrics
{
    float scale;
    int top;
    int bottom;
    int width;
};

auto as_stb_codepoint(char32_t codepoint) -> int { return static_cast<int>(codepoint); }

// Renders a codepoint as "U+XXXX" (four digits minimum, more for non-BMP code points).
auto format_codepoint(char32_t codepoint) -> std::string
{
    std::array<char, 11> buffer{}; // "U+10FFFF" plus terminator
    std::snprintf(buffer.data(), buffer.size(), "U+%04X", static_cast<unsigned int>(codepoint));
    return std::string(buffer.data());
}

auto require_glyphs(const stbtt_fontinfo &font, std::span<const char32_t> codepoints,
                    const std::string &font_path) -> void
{
    for (const char32_t codepoint : codepoints)
    {
        if (stbtt_FindGlyphIndex(&font, as_stb_codepoint(codepoint)) == 0)
        {
            throw std::runtime_error("font " + font_path + " has no glyph for " +
                                     format_codepoint(codepoint) +
                                     "; use --font to specify a font that covers this text");
        }
    }
}

auto measure_text(const stbtt_fontinfo &font, std::span<const char32_t> codepoints, int size)
    -> TextMetrics
{
    const float scale = stbtt_ScaleForPixelHeight(&font, static_cast<float>(size));
    float cursor = 0.0F;
    int top = std::numeric_limits<int>::max();
    int bottom = std::numeric_limits<int>::min();

    for (std::size_t index = 0; index < codepoints.size(); ++index)
    {
        const int codepoint = as_stb_codepoint(codepoints[index]);
        int x0 = 0;
        int y0 = 0;
        int x1 = 0;
        int y1 = 0;
        stbtt_GetCodepointBitmapBox(&font, codepoint, scale, scale, &x0, &y0, &x1, &y1);
        top = std::min(top, y0);
        bottom = std::max(bottom, y1);

        int advance = 0;
        int left_bearing = 0;
        stbtt_GetCodepointHMetrics(&font, codepoint, &advance, &left_bearing);
        cursor += static_cast<float>(advance) * scale;
        if (index + 1 < codepoints.size())
        {
            cursor += static_cast<float>(stbtt_GetCodepointKernAdvance(
                          &font, codepoint, as_stb_codepoint(codepoints[index + 1]))) *
                      scale;
        }
    }

    return {.scale = scale,
            .top = top,
            .bottom = bottom,
            .width = std::max(1, static_cast<int>(std::ceil(cursor)) + 2 * HORIZONTAL_PADDING)};
}
} // namespace

auto find_font(const std::filesystem::path &explicit_path) -> std::filesystem::path
{
    if (!explicit_path.empty())
    {
        if (!std::filesystem::is_regular_file(explicit_path))
        {
            throw std::runtime_error("font not found: " + explicit_path.string());
        }
        return explicit_path;
    }

    std::vector<std::filesystem::path> candidates{
        "/usr/share/fonts/google-droid-sans-fonts/DroidSansFallbackFull.ttf",
        "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/google-noto/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/google-noto/NotoSans-Regular.ttf",
    };
    const auto match =
        std::ranges::find_if(candidates, [](const auto &candidate) -> bool
                             { return std::filesystem::is_regular_file(candidate); });
    if (match != candidates.end())
    {
        return *match;
    }
    throw std::runtime_error("no CJK-capable font found; use --font to specify a TTF/TTC file");
}

auto render_text(const std::string &text, const std::string &font_path, int font_size,
                 int threshold) -> GlyphBitmap
{
    if (text.empty())
    {
        throw std::invalid_argument("message must not be empty");
    }
    if (font_size <= MIN_FONT_SIZE)
    {
        throw std::invalid_argument("font-size must be greater than 5");
    }
    if (threshold < 0 || threshold > 255)
    {
        throw std::invalid_argument("threshold must be between 0..255");
    }
    const auto binary_threshold = static_cast<uint8_t>(threshold);

    const auto codepoints = utf8_codepoints(text);
    const std::vector<uint8_t> font_data = read_file(font_path);
    if (font_data.empty())
    {
        throw std::runtime_error("font file is empty: " + font_path);
    }

    const int font_offset = stbtt_GetFontOffsetForIndex(font_data.data(), 0);
    if (font_offset < 0)
    {
        throw std::runtime_error("not a valid TTF/TTC font: " + font_path);
    }
    stbtt_fontinfo font{};
    if (stbtt_InitFont(&font, font_data.data(), font_offset) == 0)
    {
        throw std::runtime_error("failed to initialize font: " + font_path);
    }

    require_glyphs(font, codepoints, font_path);

    std::optional<TextMetrics> metrics;
    for (int size = font_size; size > MIN_FONT_SIZE; --size)
    {
        auto candidate = measure_text(font, codepoints, size);
        if (candidate.bottom - candidate.top <= ROWS)
        {
            metrics = candidate;
            break;
        }
    }
    if (!metrics)
    {
        throw std::runtime_error("text cannot fit within " + std::to_string(ROWS) +
                                 " pixel height");
    }

    const auto width = static_cast<std::size_t>(metrics->width);
    std::vector<uint8_t> pixels(width * static_cast<std::size_t>(ROWS), DARK);
    auto cursor = static_cast<float>(HORIZONTAL_PADDING);
    const int vertical_origin = ((ROWS - (metrics->bottom - metrics->top)) / 2) - metrics->top;

    for (std::size_t index = 0; index < codepoints.size(); ++index)
    {
        const int codepoint = as_stb_codepoint(codepoints[index]);
        int glyph_width = 0;
        int glyph_height = 0;
        int x_offset = 0;
        int y_offset = 0;
        StbBitmap glyph(stbtt_GetCodepointBitmap(&font, metrics->scale, metrics->scale, codepoint,
                                                 &glyph_width, &glyph_height, &x_offset,
                                                 &y_offset));

        const int glyph_start_x = static_cast<int>(std::floor(cursor)) + x_offset;
        const int glyph_start_y = vertical_origin + y_offset;
        for (int glyph_y = 0; glyph_y < glyph_height; ++glyph_y)
        {
            const int pixel_y = glyph_start_y + glyph_y;
            if (pixel_y < 0 || pixel_y >= ROWS)
            {
                continue;
            }
            for (int glyph_x = 0; glyph_x < glyph_width; ++glyph_x)
            {
                const int pixel_x = glyph_start_x + glyph_x;
                if (pixel_x < 0 || pixel_x >= metrics->width)
                {
                    continue;
                }
                const auto glyph_index =
                    (static_cast<std::size_t>(glyph_y) * static_cast<std::size_t>(glyph_width)) +
                    static_cast<std::size_t>(glyph_x);
                if (glyph != nullptr && glyph.get()[glyph_index] >= binary_threshold)
                {
                    const auto pixel_index = (static_cast<std::size_t>(pixel_y) * width) +
                                             static_cast<std::size_t>(pixel_x);
                    pixels[pixel_index] = BRIGHT;
                }
            }
        }

        int advance = 0;
        int left_bearing = 0;
        stbtt_GetCodepointHMetrics(&font, codepoint, &advance, &left_bearing);
        cursor += static_cast<float>(advance) * metrics->scale;
        if (index + 1 < codepoints.size())
        {
            cursor += static_cast<float>(stbtt_GetCodepointKernAdvance(
                          &font, codepoint, as_stb_codepoint(codepoints[index + 1]))) *
                      metrics->scale;
        }
    }

    return {.pixels = std::move(pixels), .width = metrics->width};
}

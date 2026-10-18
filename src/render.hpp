#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <ostream>
#include <string>

#include "constants.hpp"
#include "types.hpp"

struct RenderOptions
{
    std::filesystem::path font{};
    std::filesystem::path vendor_font_dir{};
    bool no_vendor_font{false};
    int font_size{ROWS};
    int threshold{STB_THRESHOLD};
};

enum class FontSource : std::uint8_t
{
    vendor,
    outline,
};

struct RenderInfo
{
    FontSource source;
    std::filesystem::path font_path; // vendor directory or the actual outline file
    std::string fallback_reason;     // empty unless the vendor glyph path fell back
};

struct RenderedMessage
{
    GlyphBitmap image;
    RenderInfo font;
};

// Owns one batch of font state. Not thread-safe: construct one per rendering batch and do not
// share it across threads. diagnostics is borrowed and must outlive the renderer.
class TextRenderer
{
  public:
    explicit TextRenderer(RenderOptions options, std::ostream *diagnostics = nullptr);
    auto render(const std::string &text) -> RenderedMessage;

  private:
    RenderOptions options_;
    std::ostream *diagnostics_{nullptr};
    std::optional<VendorFont> vendor_font_;
    std::filesystem::path vendor_font_dir_;
    std::optional<std::filesystem::path> outline_font_;
};

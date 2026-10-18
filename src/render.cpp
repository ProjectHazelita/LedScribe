#include "render.hpp"

#include <stdexcept>
#include <utility>

#include "truetype.hpp"
#include "vendor_font.hpp"

namespace
{
constexpr int MIN_FONT_SIZE = 5;

auto validate_options(const RenderOptions &options) -> void
{
    if (options.font_size <= MIN_FONT_SIZE)
    {
        throw std::invalid_argument("font-size must be greater than 5");
    }
    if (options.threshold < 0 || options.threshold > 255)
    {
        throw std::invalid_argument("threshold must be between 0..255");
    }
}
} // namespace

TextRenderer::TextRenderer(RenderOptions options, std::ostream *diagnostics)
    : options_(std::move(options)), diagnostics_(diagnostics)
{
    validate_options(options_);
    if (!options_.font.empty() || options_.no_vendor_font)
    {
        return;
    }

    vendor_font_dir_ = find_vendor_font_dir(options_.vendor_font_dir);
    if (vendor_font_dir_.empty())
    {
        if (diagnostics_ != nullptr)
        {
            *diagnostics_ << "vendor/ASC11 and vendor/HZK11 not found; using outline font\n";
        }
        return;
    }

    // Load once per batch; every message in the batch reuses the same vendor glyph data.
    vendor_font_ = load_vendor_font(vendor_font_dir_);
    if (diagnostics_ != nullptr)
    {
        *diagnostics_ << "using vendor bitmap font: " << vendor_font_dir_ << '\n';
    }
}

auto TextRenderer::render(const std::string &text) -> RenderedMessage
{
    std::string fallback_reason;
    if (vendor_font_)
    {
        try
        {
            return RenderedMessage{.image = render_vendor(text, *vendor_font_),
                                   .font = RenderInfo{.source = FontSource::vendor,
                                                      .font_path = vendor_font_dir_,
                                                      .fallback_reason = {}}};
        }
        catch (const UnsupportedVendorGlyph &error)
        {
            // Only an unsupported vendor glyph may fall back; other failures stay errors.
            fallback_reason = error.what();
            if (diagnostics_ != nullptr)
            {
                *diagnostics_ << fallback_reason
                              << "; falling back to outline font for this message\n";
            }
        }
    }

    if (!outline_font_)
    {
        outline_font_ = find_font(options_.font);
        if (diagnostics_ != nullptr)
        {
            *diagnostics_ << "using outline font: " << *outline_font_ << '\n';
        }
    }

    return RenderedMessage{
        .image = render_text(text, outline_font_->string(), options_.font_size, options_.threshold),
        .font = RenderInfo{.source = FontSource::outline,
                           .font_path = *outline_font_,
                           .fallback_reason = std::move(fallback_reason)}};
}

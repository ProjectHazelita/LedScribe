#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "constants.hpp"
#include "render.hpp"
#include "utils.hpp"

namespace
{
auto require(bool condition, std::string_view message) -> void
{
    if (!condition) throw std::runtime_error(std::string(message));
}

auto require_contains(std::string_view text, std::string_view needle, std::string_view message)
    -> void
{
    require(text.find(needle) != std::string_view::npos, message);
}

template <typename Exception, typename Function>
auto require_throws(Function &&function, std::string_view message) -> void
{
    try
    {
        function();
    }
    catch (const Exception &)
    {
        return;
    }
    catch (const std::exception &error)
    {
        throw std::runtime_error(std::string(message) + ": unexpected error: " + error.what());
    }
    throw std::runtime_error(std::string(message) + ": no exception thrown");
}

template <typename Exception, typename Function>
auto require_throws_containing(Function &&function, std::string_view needle,
                               std::string_view message) -> void
{
    try
    {
        function();
    }
    catch (const Exception &error)
    {
        require_contains(error.what(), needle, message);
        return;
    }
    catch (const std::exception &error)
    {
        throw std::runtime_error(std::string(message) + ": unexpected error: " + error.what());
    }
    throw std::runtime_error(std::string(message) + ": no exception thrown");
}

class ScratchDirectory
{
  public:
    ScratchDirectory() : previous_(std::filesystem::current_path())
    {
        auto pattern =
            (std::filesystem::temp_directory_path() / "ledscribe-render-XXXXXX").string();
        if (mkdtemp(pattern.data()) == nullptr)
            throw std::runtime_error("failed to create temporary render directory");
        path_ = pattern;
    }

    ~ScratchDirectory()
    {
        std::error_code error;
        std::filesystem::current_path(previous_, error);
        std::filesystem::remove_all(path_, error);
    }

    ScratchDirectory(const ScratchDirectory &) = delete;
    auto operator=(const ScratchDirectory &) -> ScratchDirectory & = delete;

    [[nodiscard]] auto path() const -> const std::filesystem::path & { return path_; }

  private:
    std::filesystem::path previous_;
    std::filesystem::path path_;
};

struct SyntheticFonts
{
    std::vector<uint8_t> ascii;
    std::vector<uint8_t> gbk;
};

auto ascii_row_byte(std::size_t code, int row) -> uint8_t
{
    const auto value = (code * 17U) + (static_cast<std::size_t>(row) * 13U) + 1U;
    return static_cast<uint8_t>(value & 0xFFU);
}

auto make_vendor_fonts() -> SyntheticFonts
{
    SyntheticFonts fonts{.ascii = std::vector<uint8_t>(ASCII_FONT_SIZE),
                         .gbk = std::vector<uint8_t>(GBK_FONT_SIZE)};
    for (std::size_t code = 0; code < 256U; ++code)
    {
        for (int row = 0; row < ROWS; ++row)
        {
            fonts.ascii[(code * static_cast<std::size_t>(ROWS)) +
                        static_cast<std::size_t>(row)] = ascii_row_byte(code, row);
        }
    }
    for (std::size_t index = 0; index < fonts.gbk.size(); ++index)
    {
        fonts.gbk[index] = static_cast<uint8_t>((index * 31U + 7U) & 0xFFU);
    }
    return fonts;
}

auto write_file(const std::filesystem::path &path, const std::vector<uint8_t> &bytes) -> void
{
    std::ofstream file(path, std::ios::binary);
    require(static_cast<bool>(file), "failed to create font fixture");
    file.write(reinterpret_cast<const char *>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
    require(static_cast<bool>(file), "failed to write font fixture");
}

// Writes both synthetic bitmap font files and returns the exact bytes used.
auto write_vendor_fonts(const std::filesystem::path &directory) -> SyntheticFonts
{
    std::filesystem::create_directories(directory);
    auto fonts = make_vendor_fonts();
    write_file(directory / "ASC11", fonts.ascii);
    write_file(directory / "HZK11", fonts.gbk);
    return fonts;
}

auto expected_ascii_pixel(const SyntheticFonts &fonts, std::size_t code, int row, int column)
    -> int
{
    const auto bits =
        fonts.ascii[(code * static_cast<std::size_t>(ROWS)) + static_cast<std::size_t>(row)];
    const auto shift = ASCII_GLYPH_WIDTH - 1 - column;
    return ((bits >> shift) & 1U) != 0U ? BRIGHT : DARK;
}

auto gbk_glyph_index(std::string_view text) -> std::size_t
{
    const auto encoded = utf8_to_gbk(text);
    require(encoded.size() == 2U, "fixture text must encode to a single GBK character");
    return (static_cast<std::size_t>(encoded[0] - GBK_LEAD_MIN) *
            static_cast<std::size_t>(GBK_TRAIL_COUNT)) +
           static_cast<std::size_t>(encoded[1] - GBK_TRAIL_MIN);
}

auto expected_gbk_pixel(const SyntheticFonts &fonts, std::size_t glyph_index, int row, int column)
    -> int
{
    const auto offset =
        (glyph_index * static_cast<std::size_t>(GBK_GLYPH_SIZE)) +
        (static_cast<std::size_t>(row) * static_cast<std::size_t>(GBK_BYTES_PER_ROW));
    const auto row_data = static_cast<uint16_t>(
        (static_cast<uint16_t>(fonts.gbk[offset]) << 8) | fonts.gbk[offset + 1]);
    const auto mask = static_cast<uint16_t>(1U << (GBK_STORAGE_WIDTH - 1 - column));
    return static_cast<unsigned int>(row_data & mask) != 0U ? BRIGHT : DARK;
}

auto require_ascii_glyph(const GlyphBitmap &image, const SyntheticFonts &fonts, std::size_t code,
                         int glyph_x) -> void
{
    for (int row = 0; row < ROWS; ++row)
    {
        for (int column = 0; column < ASCII_GLYPH_WIDTH; ++column)
        {
            const auto pixel =
                (static_cast<std::size_t>(row) * static_cast<std::size_t>(image.width)) +
                (static_cast<std::size_t>(glyph_x) + static_cast<std::size_t>(column));
            require(image.pixels[pixel] == expected_ascii_pixel(fonts, code, row, column),
                    "ASCII pixels must match the synthetic vendor glyph");
        }
    }
}

auto require_gbk_glyph(const GlyphBitmap &image, const SyntheticFonts &fonts,
                       std::size_t glyph_index) -> void
{
    for (int row = 0; row < ROWS; ++row)
    {
        for (int column = 0; column < GBK_GLYPH_WIDTH; ++column)
        {
            const auto pixel =
                (static_cast<std::size_t>(row) * static_cast<std::size_t>(image.width)) +
                static_cast<std::size_t>(column);
            require(image.pixels[pixel] == expected_gbk_pixel(fonts, glyph_index, row, column),
                    "GBK pixels must match the synthetic vendor glyph");
        }
    }
}

auto test_constructor_validation() -> void
{
    const ScratchDirectory scratch;
    const auto vendor = scratch.path() / "vendor";
    static_cast<void>(write_vendor_fonts(vendor));

    std::ostringstream diagnostics;
    require_throws<std::invalid_argument>(
        [&]() -> void
        {
            static_cast<void>(TextRenderer(
                RenderOptions{.vendor_font_dir = vendor, .font_size = 5}, &diagnostics));
        },
        "font size 5 must be rejected even when the vendor font is used");
    require(diagnostics.str().empty(), "option validation must precede font discovery");

    require_throws<std::invalid_argument>(
        [&]() -> void
        {
            static_cast<void>(TextRenderer(
                RenderOptions{.vendor_font_dir = vendor, .threshold = -1}, &diagnostics));
        },
        "negative threshold must be rejected even when the vendor font is used");
    require(diagnostics.str().empty(), "option validation must precede font discovery");

    require_throws<std::invalid_argument>(
        [&]() -> void
        {
            static_cast<void>(TextRenderer(
                RenderOptions{.vendor_font_dir = vendor, .threshold = 256}, &diagnostics));
        },
        "threshold above 255 must be rejected even when the vendor font is used");
    require(diagnostics.str().empty(), "option validation must precede font discovery");

    TextRenderer lower_boundary(RenderOptions{.vendor_font_dir = vendor,
                                              .font_size = 6,
                                              .threshold = 0},
                                nullptr);
    require(lower_boundary.render("A").image.width == ASCII_GLYPH_WIDTH,
            "minimum valid options must render");

    TextRenderer upper_boundary(RenderOptions{.vendor_font_dir = vendor, .threshold = 255},
                                nullptr);
    require(upper_boundary.render("A").image.width == ASCII_GLYPH_WIDTH,
            "maximum threshold must render");
}

auto test_vendor_source_and_pixels() -> void
{
    const ScratchDirectory scratch;
    const auto vendor = scratch.path() / "vendor";
    const auto fonts = write_vendor_fonts(vendor);

    std::ostringstream diagnostics;
    TextRenderer renderer(RenderOptions{.vendor_font_dir = vendor}, &diagnostics);
    require_contains(diagnostics.str(), "using vendor bitmap font: ",
                     "vendor batch must report its source once");

    const auto rendered = renderer.render("AB");
    require(rendered.font.source == FontSource::vendor,
            "vendor rendering must report the vendor source");
    require(rendered.font.font_path == vendor,
            "vendor font metadata must point at the vendor directory");
    require(rendered.font.fallback_reason.empty(),
            "vendor rendering must not report a fallback reason");
    require(rendered.image.width == 2 * ASCII_GLYPH_WIDTH,
            "two ASCII glyphs must be eight columns each");
    rendered.image.validate();
    require_ascii_glyph(rendered.image, fonts, static_cast<std::size_t>('A'), 0);
    require_ascii_glyph(rendered.image, fonts, static_cast<std::size_t>('B'), ASCII_GLYPH_WIDTH);

    const auto chinese = renderer.render("洛");
    require(chinese.font.source == FontSource::vendor,
            "GBK rendering must report the vendor source");
    require(chinese.font.fallback_reason.empty(),
            "GBK rendering must not report a fallback reason");
    require(chinese.image.width == GBK_GLYPH_WIDTH, "GBK glyph must be eleven columns wide");
    chinese.image.validate();
    require_gbk_glyph(chinese.image, fonts, gbk_glyph_index("洛"));

    TextRenderer silent(RenderOptions{.vendor_font_dir = vendor}, nullptr);
    require(silent.render("A").image.width == ASCII_GLYPH_WIDTH,
            "a null diagnostics stream must not affect rendering");
}

auto test_auto_vendor_discovery() -> void
{
    const ScratchDirectory scratch;
    const auto vendor = scratch.path() / "vendor";
    static_cast<void>(write_vendor_fonts(vendor));
    std::filesystem::current_path(scratch.path());

    std::ostringstream diagnostics;
    TextRenderer renderer(RenderOptions{}, &diagnostics);
    const auto rendered = renderer.render("A");
    require(rendered.font.source == FontSource::vendor,
            "automatic discovery must use the vendor font");
    require(rendered.font.font_path == std::filesystem::current_path() / "vendor",
            "automatic discovery metadata must point at cwd/vendor");
    require(rendered.font.fallback_reason.empty(),
            "automatic vendor rendering must not report a fallback reason");
    require_contains(diagnostics.str(), "using vendor bitmap font: ",
                     "automatic discovery must report the vendor source");
}

auto test_explicit_bad_vendor_is_not_bypassed() -> void
{
    const ScratchDirectory scratch;
    static_cast<void>(write_vendor_fonts(scratch.path() / "vendor"));
    std::filesystem::current_path(scratch.path());

    const auto broken = scratch.path() / "broken";
    std::filesystem::create_directories(broken);
    write_file(broken / "ASC11", std::vector<uint8_t>(ASCII_FONT_SIZE));

    std::ostringstream diagnostics;
    require_throws_containing<std::runtime_error>(
        [&]() -> void
        {
            static_cast<void>(TextRenderer(RenderOptions{.vendor_font_dir = broken}, &diagnostics));
        },
        "HZK11",
        "an explicitly incomplete vendor directory must fail instead of auto-detecting cwd/vendor");
    require(diagnostics.str().empty(),
            "a broken explicit vendor directory must not emit a vendor diagnostic");
}

auto test_explicit_outline_priority() -> void
{
    const ScratchDirectory scratch;
    static_cast<void>(write_vendor_fonts(scratch.path() / "vendor"));

    const auto missing_outline = scratch.path() / "missing-outline.ttf";
    std::ostringstream diagnostics;
    TextRenderer renderer(
        RenderOptions{.font = missing_outline, .vendor_font_dir = scratch.path() / "vendor"},
        &diagnostics);
    require(diagnostics.str().empty(),
            "an explicit outline font must skip vendor discovery diagnostics");
    require_throws_containing<std::runtime_error>(
        [&]() -> void { static_cast<void>(renderer.render("A")); },
        "font not found:",
        "an explicit outline font must take precedence over the available vendor font");
}

auto test_no_vendor_font_disables_vendor() -> void
{
    const ScratchDirectory scratch;
    // A broken explicit directory would throw if discovery ran; no_vendor_font must skip it.
    const auto broken = scratch.path() / "absent";
    std::ostringstream diagnostics;
    static_cast<void>(
        TextRenderer(RenderOptions{.vendor_font_dir = broken, .no_vendor_font = true},
                     &diagnostics));
    require(diagnostics.str().empty(),
            "disabling the vendor font must not inspect the vendor directory");
}

auto test_fallback_only_for_unsupported_vendor_glyph() -> void
{
    const ScratchDirectory scratch;
    static_cast<void>(write_vendor_fonts(scratch.path() / "vendor"));
    std::filesystem::current_path(scratch.path());

    std::ostringstream diagnostics;
    TextRenderer renderer(RenderOptions{}, &diagnostics);
    // U+1F642 has no GBK representation; only UnsupportedVendorGlyph may reach the outline path.
    try
    {
        static_cast<void>(renderer.render("\xF0\x9F\x99\x82"));
    }
    catch (const std::exception &)
    {
        // The outline fallback is host-dependent; the diagnostic below proves it was attempted.
    }
    const auto fallback = diagnostics.str();
    require_contains(fallback, "falling back to outline font for this message",
                     "an unsupported vendor glyph must fall back to the outline path");
    const auto vendor_position = fallback.find("using vendor bitmap font:");
    const auto fallback_position = fallback.find("falling back to outline font");
    require(vendor_position != std::string::npos && vendor_position < fallback_position,
            "vendor diagnostics must precede the fallback diagnostic");

    std::ostringstream validation_diagnostics;
    TextRenderer validation(RenderOptions{}, &validation_diagnostics);
    require_throws<std::invalid_argument>(
        [&]() -> void { static_cast<void>(validation.render("")); },
        "empty text must keep its validation error");
    require(validation_diagnostics.str().find("falling back") == std::string::npos,
            "non-UnsupportedVendorGlyph failures must not trigger the outline path");
}
} // namespace

auto main() -> int
{
    try
    {
        test_constructor_validation();
        test_vendor_source_and_pixels();
        test_auto_vendor_discovery();
        test_explicit_bad_vendor_is_not_bypassed();
        test_explicit_outline_priority();
        test_no_vendor_font_disables_vendor();
        test_fallback_only_for_unsupported_vendor_glyph();
        std::cout << "render tests passed\n";
        return 0;
    }
    catch (const std::exception &error)
    {
        std::cerr << "test failed: " << error.what() << '\n';
        return 1;
    }
}

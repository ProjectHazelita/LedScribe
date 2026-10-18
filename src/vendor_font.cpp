#include "vendor_font.hpp"

#include "constants.hpp"
#include "utils.hpp"

namespace
{
auto require_font_size(std::string_view name, std::size_t actual, std::size_t expected) -> void
{
    if (actual != expected)
    {
        throw std::runtime_error(std::string(name) + " has unexpected size: " +
                                 std::to_string(actual) + ", expected " + std::to_string(expected));
    }
}

// Comma-separated names of the font files missing from directory ("" when both are present).
auto missing_vendor_files(const std::filesystem::path &directory) -> std::string
{
    std::string missing;
    for (const char *name : {"ASC11", "HZK11"})
    {
        if (std::filesystem::is_regular_file(directory / name))
        {
            continue;
        }
        if (!missing.empty())
        {
            missing += ", ";
        }
        missing += name;
    }
    return missing;
}
} // namespace

auto load_vendor_font(const std::filesystem::path &directory) -> VendorFont
{
    VendorFont font{.ascii = read_file((directory / "ASC11").string()),
                    .gbk = read_file((directory / "HZK11").string())};
    require_font_size("ASC11", font.ascii.size(), ASCII_FONT_SIZE);
    require_font_size("HZK11", font.gbk.size(), GBK_FONT_SIZE);
    return font;
}

auto find_vendor_font_dir(const std::filesystem::path &explicit_directory) -> std::filesystem::path
{
    if (!explicit_directory.empty())
    {
        const auto missing = missing_vendor_files(explicit_directory);
        if (!missing.empty())
        {
            throw std::runtime_error("vendor font directory " + explicit_directory.string() +
                                     " is missing " + missing +
                                     "; point --vendor-font-dir at a directory holding ASC11 and "
                                     "HZK11, or omit it to auto-detect");
        }
        return explicit_directory;
    }

    const auto detected = std::filesystem::current_path() / "vendor";
    return missing_vendor_files(detected).empty() ? detected : std::filesystem::path{};
}

auto render_vendor(const std::string &text, const VendorFont &font) -> GlyphBitmap
{
    if (text.empty())
    {
        throw std::invalid_argument("message must not be empty");
    }
    require_font_size("ASC11", font.ascii.size(), ASCII_FONT_SIZE);
    require_font_size("HZK11", font.gbk.size(), GBK_FONT_SIZE);

    std::vector<uint8_t> gbk_bytes;
    try
    {
        gbk_bytes = utf8_to_gbk(text);
    }
    catch (const EncodingError &error)
    {
        throw UnsupportedVendorGlyph(error.what());
    }

    // First pass: compute total pixel width
    int total_width = 0;
    size_t i = 0;
    while (i < gbk_bytes.size())
    {
        uint8_t byte0 = gbk_bytes[i++];
        total_width += (byte0 < GBK_LEAD_MIN) ? ASCII_GLYPH_WIDTH : GBK_GLYPH_WIDTH;
        if (byte0 >= GBK_LEAD_MIN)
        {
            if (i >= gbk_bytes.size())
            {
                throw UnsupportedVendorGlyph("truncated GBK character");
            }
            ++i;
        }
    }

    // Second pass: render into pixel buffer
    const auto pixel_count = static_cast<std::size_t>(total_width) * static_cast<std::size_t>(ROWS);
    std::vector<uint8_t> pixels(pixel_count, DARK);
    int cursor = 0;
    i = 0;
    while (i < gbk_bytes.size())
    {
        uint8_t byte0 = gbk_bytes[i++];
        if (byte0 < GBK_LEAD_MIN)
        { // ASCII glyph
            const auto offset = static_cast<std::size_t>(byte0) * static_cast<std::size_t>(ROWS);
            for (int row = 0; row < ROWS; row++)
            {
                uint8_t row_data = font.ascii[offset + static_cast<std::size_t>(row)];
                for (int x = 0; x < ASCII_GLYPH_WIDTH; x++)
                {
                    pixels[(row * total_width) + cursor + x] =
                        (((row_data >> (ASCII_GLYPH_WIDTH - 1 - x)) & 1) != 0) ? BRIGHT : DARK;
                }
            }
            cursor += ASCII_GLYPH_WIDTH;
        }
        else
        { // GBK: 11 columns
            uint8_t lead = byte0;
            if (i >= gbk_bytes.size())
            {
                throw UnsupportedVendorGlyph("truncated GBK character");
            }
            uint8_t trail = gbk_bytes[i++];
            if (lead < GBK_LEAD_MIN || lead > 0xFE || trail < GBK_TRAIL_MIN || trail > 0xFE)
            {
                throw UnsupportedVendorGlyph("GBK encoding exceeds HZK11 range");
            }
            const auto index = (static_cast<std::size_t>(lead - GBK_LEAD_MIN) *
                                static_cast<std::size_t>(GBK_TRAIL_COUNT)) +
                               static_cast<std::size_t>(trail - GBK_TRAIL_MIN);
            const auto offset = index * static_cast<std::size_t>(GBK_GLYPH_SIZE);
            for (int row = 0; row < ROWS; row++)
            {
                const auto row_offset = offset + (static_cast<std::size_t>(row) *
                                                  static_cast<std::size_t>(GBK_BYTES_PER_ROW));
                auto row_data = static_cast<uint16_t>(
                    (static_cast<uint16_t>(font.gbk[row_offset]) << 8) | font.gbk[row_offset + 1]);
                for (int x = 0; x < GBK_GLYPH_WIDTH; x++)
                {
                    pixels[(row * total_width) + cursor + x] =
                        (((row_data >> (GBK_STORAGE_WIDTH - 1 - x)) & 1) != 0) ? BRIGHT : DARK;
                }
            }
            cursor += GBK_GLYPH_WIDTH;
        }
    }
    return {.pixels = std::move(pixels), .width = total_width};
}

#include "pack.hpp"
#include "constants.hpp"

#include <stdexcept>

auto pack_image(std::span<const uint8_t> pixels, int width, int frame_width)
    -> std::vector<uint8_t>
{
    if (width <= 0)
    {
        throw std::invalid_argument("bitmap width must be greater than 0");
    }
    if (frame_width <= 0)
    {
        throw std::invalid_argument("frame width must be greater than 0");
    }
    const auto unsigned_width = static_cast<std::size_t>(width);
    if (pixels.size() != unsigned_width * static_cast<std::size_t>(ROWS))
    {
        throw std::invalid_argument("bitmap data length does not match dimensions");
    }

    const auto unsigned_frame_width = static_cast<std::size_t>(frame_width);
    const auto frames = (unsigned_width + unsigned_frame_width - 1) / unsigned_frame_width;
    const auto columns = (unsigned_frame_width + COLUMN_WIDTH - 1) / COLUMN_WIDTH;
    std::vector<uint8_t> packed;
    packed.reserve(frames * columns * static_cast<std::size_t>(ROWS));

    for (std::size_t frame = 0; frame < frames; ++frame)
    {
        const auto frame_start = frame * unsigned_frame_width;
        for (std::size_t col = 0; col < columns; ++col)
        {
            for (int row = 0; row < ROWS; ++row)
            {
                uint8_t value = 0;
                for (int bit = 0; bit < COLUMN_WIDTH; ++bit)
                {
                    const auto frame_x = (col * COLUMN_WIDTH) + static_cast<std::size_t>(bit);
                    const auto x = frame_start + frame_x;
                    if (frame_x >= unsigned_frame_width || x >= unsigned_width)
                    {
                        break;
                    }
                    const auto pixel_index =
                        (static_cast<std::size_t>(row) * unsigned_width) + x;
                    if (pixels[pixel_index] > BRIGHTNESS_DIV)
                    {
                        value |= static_cast<uint8_t>(1U << (COLUMN_WIDTH - 1 - bit));
                    }
                }
                packed.push_back(value);
            }
        }
    }
    return packed;
}

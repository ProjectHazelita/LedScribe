#pragma once

#include <cstdint>
#include <span>
#include <vector>

// Split into logical frames before padding each frame to an 8-pixel column group.
// For a continuous bitmap, pass frame_width == width.
auto pack_image(std::span<const uint8_t> pixels, int width, int frame_width)
    -> std::vector<uint8_t>;

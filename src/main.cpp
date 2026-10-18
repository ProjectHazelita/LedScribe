#include <array>
#include <charconv>
#include <cstdint>
#include <filesystem>
#include <getopt.h>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "constants.hpp"
#include "device.hpp"
#include "protocol.hpp"
#include "render.hpp"

namespace
{
struct Options
{
    std::vector<std::string> messages;
    bool list_devices{false};
    bool show_help{false};
    std::optional<std::string> device;
    bool dry_run{false};
    bool show_preview{false};
    bool verbose{false};
    std::filesystem::path font;
    std::filesystem::path vendor_font_dir;
    bool no_vendor_font{false};
    int font_size{ROWS};
    int threshold{STB_THRESHOLD};
    DisplaySettings display;
};

auto print_help(std::ostream &output) -> void
{
    output << R"(Usage: ledscribe [OPTIONS] MESSAGE [MESSAGE ...]

Write 1..8 cycling messages to a 0416:5020 CH546 11×44 LED display.

Options:
  --list-devices            List compatible devices only
  --device ID               Specify BUS:DEV:OUT:IN
  --dry-run                 Encode only, do not write to the display
  --preview                 Preview the 11-pixel bitmap in the terminal
  --font PATH               Use the specified TTF/TTC outline font
  --vendor-font-dir PATH    Specify the directory containing ASC11/HZK11
  --no-vendor-font          Disable the vendor bitmap font
  --font-size N             Initial font size (default: 11)
  --threshold N             Binarization threshold 0..255 (default: 96)
  --speed N[,N...]          Speed 1..8 per message, comma-separated (default: 4)
  --mode MODE[,MODE...]     Display mode per message, comma-separated (default: left)
                           One speed/mode value applies to every message
                           Multiple values configure slots in order (at most 8)
                           Modes: left, right, up, down, still, animation,
                                  drop, curtain, laser (or numeric 0..8)
  --brightness N            25, 50, 75 or 100 (default: 50)
  --verbose                Print USB diagnostics to stderr
  --blink SLOT[,SLOT...]    Enable blink for the given message slots (1..8 or all)
  --border SLOT[,SLOT...]   Enable border for the given message slots (1..8 or all)
  -h, --help                Show this help
)";
}

auto parse_integer(std::string_view text, std::string_view name) -> int
{
    int value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size())
    {
        throw std::invalid_argument(std::string(name) + " must be an integer");
    }
    return value;
}

auto split_values(std::string_view text, std::string_view name) -> std::vector<std::string_view>
{
    std::vector<std::string_view> parts;
    std::size_t start = 0;
    while (start <= text.size())
    {
        auto end = text.find(',', start);
        if (end == std::string_view::npos) end = text.size();
        auto part = text.substr(start, end - start);
        auto first = part.find_first_not_of(" \t");
        auto last = part.find_last_not_of(" \t");
        if (first == std::string_view::npos)
        {
            throw std::invalid_argument(std::string(name) + " list must not contain empty values");
        }
        parts.push_back(part.substr(first, last - first + 1));
        if (parts.size() > MAX_MESSAGES)
        {
            throw std::invalid_argument(std::string(name) + " list must contain at most 8 values");
        }
        start = end + 1;
    }
    return parts;
}

auto parse_slot_indices(std::string_view text) -> std::vector<std::size_t>
{
    if (text == "all")
    {
        std::vector<std::size_t> all(MAX_MESSAGES);
        for (std::size_t i = 0; i < MAX_MESSAGES; ++i) all[i] = i;
        return all;
    }
    auto parts = split_values(text, "slot");
    std::vector<std::size_t> indices;
    for (auto part : parts)
    {
        int idx = parse_integer(part, "slot number");
        if (idx < 1 || idx > MAX_MESSAGES)
            throw std::invalid_argument("slot number must be between 1..8");
        indices.push_back(static_cast<std::size_t>(idx - 1));
    }
    return indices;
}

auto parse_options(int argc, char **argv) -> Options
{
    enum OptionCode : uint16_t
    {
        list_devices = 1000,
        device,
        dry_run,
        preview,
        verbose,
        font,
        vendor_font_dir,
        no_vendor_font,
        font_size,
        threshold,
        speed,
        mode,
        brightness,
        blink,
        border,
    };
    const std::array<option, 17> options = {
        option{
            .name = "list-devices", .has_arg = no_argument, .flag = nullptr, .val = list_devices},
        option{.name = "device", .has_arg = required_argument, .flag = nullptr, .val = device},
        option{.name = "dry-run", .has_arg = no_argument, .flag = nullptr, .val = dry_run},
        option{.name = "preview", .has_arg = no_argument, .flag = nullptr, .val = preview},
        option{.name = "verbose", .has_arg = no_argument, .flag = nullptr, .val = verbose},
        option{.name = "font", .has_arg = required_argument, .flag = nullptr, .val = font},
        option{.name = "vendor-font-dir",
               .has_arg = required_argument,
               .flag = nullptr,
               .val = vendor_font_dir},
        option{.name = "no-vendor-font",
               .has_arg = no_argument,
               .flag = nullptr,
               .val = no_vendor_font},
        option{
            .name = "font-size", .has_arg = required_argument, .flag = nullptr, .val = font_size},
        option{
            .name = "threshold", .has_arg = required_argument, .flag = nullptr, .val = threshold},
        option{.name = "speed", .has_arg = required_argument, .flag = nullptr, .val = speed},
        option{.name = "mode", .has_arg = required_argument, .flag = nullptr, .val = mode},
        option{
            .name = "brightness", .has_arg = required_argument, .flag = nullptr, .val = brightness},
        option{.name = "blink", .has_arg = required_argument, .flag = nullptr, .val = blink},
        option{.name = "border", .has_arg = required_argument, .flag = nullptr, .val = border},
        option{.name = "help", .has_arg = no_argument, .flag = nullptr, .val = 'h'},
        option{.name = nullptr, .has_arg = 0, .flag = nullptr, .val = 0},
    };

    Options result;
    opterr = 0;
    while (true)
    {
        const int parsed =
            getopt_long(argc, argv, "h", options.data(), nullptr); // NOLINT(concurrency-mt-unsafe)
        if (parsed == -1)
        {
            break;
        }
        switch (parsed)
        {
        case list_devices:
            result.list_devices = true;
            break;
        case device:
            result.device = optarg;
            break;
        case dry_run:
            result.dry_run = true;
            break;
        case preview:
            result.show_preview = true;
            break;
        case verbose:
            result.verbose = true;
            break;
        case font:
            result.font = optarg;
            break;
        case vendor_font_dir:
            result.vendor_font_dir = optarg;
            break;
        case no_vendor_font:
            result.no_vendor_font = true;
            break;
        case font_size:
            result.font_size = parse_integer(optarg, "font-size");
            if (result.font_size <= 5)
                throw std::invalid_argument("font-size must be greater than 5");
            break;
        case threshold:
            result.threshold = parse_integer(optarg, "threshold");
            if (result.threshold < 0 || result.threshold > 255)
                throw std::invalid_argument("threshold must be between 0..255");
            break;
        case speed:
        {
            auto parts = split_values(optarg, "speed");
            if (parts.size() == 1)
            {
                int s = parse_integer(parts[0], "speed");
                if (s < 1 || s > 8)
                    throw std::invalid_argument("speed must be between 1..8");
                for (auto &slot : result.display.slots) slot.speed = s;
            }
            else
            {
                for (std::size_t i = 0; i < parts.size(); ++i)
                {
                    int s = parse_integer(parts[i], "speed");
                    if (s < 1 || s > 8)
                        throw std::invalid_argument("speed must be between 1..8");
                    result.display.slots[i].speed = s;
                }
            }
            break;
        }
        case mode:
        {
            auto parts = split_values(optarg, "mode");
            if (parts.size() == 1)
            {
                DisplayMode m = parse_display_mode(parts[0]);
                for (auto &slot : result.display.slots) slot.mode = m;
            }
            else
            {
                for (std::size_t i = 0; i < parts.size(); ++i)
                    result.display.slots[i].mode = parse_display_mode(parts[i]);
            }
            break;
        }
        case brightness:
            result.display.brightness = parse_integer(optarg, "brightness");
            if (result.display.brightness != 25 && result.display.brightness != 50 &&
                result.display.brightness != 75 && result.display.brightness != 100)
                throw std::invalid_argument("brightness must be 25, 50, 75 or 100");
            break;
        case blink:
            for (auto idx : parse_slot_indices(optarg)) result.display.slots[idx].blink = true;
            break;
        case border:
            for (auto idx : parse_slot_indices(optarg)) result.display.slots[idx].border = true;
            break;
        case 'h':
            result.show_help = true;
            break;
        default:
            throw std::invalid_argument("invalid option or missing argument: " +
                                        std::string(argv[optind - 1]) + "; use --help");
        }
    }
    for (int index = optind; index < argc; ++index)
    {
        result.messages.emplace_back(argv[index]);
    }
    return result;
}

auto list_devices(std::ostream *log = nullptr) -> int
{
    const auto devices = discover_devices(log);
    if (devices.empty())
    {
        std::cout << "No 0416:5020 devices found\n";
        return 1;
    }
    for (const auto &device : devices)
    {
        std::cout << device.device_id() << "  " << device.description() << '\n';
        if (!device.discovery_error.empty())
        {
            std::cout << "  unavailable: " << device.discovery_error << '\n';
        }
    }
    return 0;
}


auto run(const Options &options) -> int
{
    auto *const log = options.verbose ? &std::cerr : nullptr;
    if (options.show_help)
    {
        print_help(std::cout);
        return 0;
    }
    if (options.list_devices)
    {
        return list_devices(log);
    }
    if (options.messages.empty() || options.messages.size() > MAX_MESSAGES)
    {
        throw std::invalid_argument("provide 1..8 messages, or use --list-devices");
    }

    TextRenderer renderer(
        RenderOptions{.font = options.font,
                      .vendor_font_dir = options.vendor_font_dir,
                      .no_vendor_font = options.no_vendor_font,
                      .font_size = options.font_size,
                      .threshold = options.threshold},
        &std::cout);
    std::vector<GlyphBitmap> images;
    images.reserve(options.messages.size());
    for (const auto &message : options.messages)
    {
        images.push_back(renderer.render(message).image);
    }

    if (options.show_preview)
    {
        for (std::size_t index = 0; index < images.size(); ++index)
        {
            std::cout << '[' << index + 1 << "] \"" << options.messages[index] << "\" ("
                      << images[index].width << "×" << ROWS << ")\n"
                      << preview(images[index]) << '\n';
        }
    }

    const auto payload = build_payload(images, options.display);
    std::cout << "encoding complete: " << payload.size() << " bytes, "
              << payload.size() / REPORT_SIZE << " CH546 reports\n";
    if (options.dry_run)
    {
        return 0;
    }

    const auto selector = options.device ? std::optional<std::string_view>(*options.device)
                                         : std::optional<std::string_view>{};
    const auto devices = discover_devices(log);
    const auto selected = select_device(devices, selector);
    auto device = BadgeDevice::open(selected, log);
    std::cout << "writing to: " << device.description() << '\n';
    device.write(payload, WRITE_DELAY, TRANSFER_TIMEOUT, log);
    std::cout << "write complete. The content should now be on screen; unplug USB to redisplay.\n";
    return 0;
}
} // namespace

auto main(int argc, char **argv) -> int
{
    try
    {
        return run(parse_options(argc, argv));
    }
    catch (const std::exception &error)
    {
        std::cerr << "error: " << error.what() << '\n';
        return 1;
    }
}

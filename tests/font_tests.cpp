#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

#include "vendor_font.hpp"

namespace
{
auto require(bool condition, std::string_view message) -> void
{
    if (!condition) throw std::runtime_error(std::string(message));
}

class ScratchDirectory
{
  public:
    ScratchDirectory() : previous_(std::filesystem::current_path())
    {
        auto pattern = (std::filesystem::temp_directory_path() / "ledscribe-font-XXXXXX").string();
        if (mkdtemp(pattern.data()) == nullptr)
            throw std::runtime_error("failed to create temporary font directory");
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

auto create_font_files(const std::filesystem::path &directory) -> void
{
    std::filesystem::create_directories(directory);
    for (const char *name : {"ASC11", "HZK11"})
    {
        // Discovery only needs file presence; loading/size checks have separate coverage.
        std::ofstream file(directory / name);
        require(static_cast<bool>(file), "failed to create font discovery fixture");
    }
}

auto test_vendor_font_discovery() -> void
{
    const ScratchDirectory scratch;
    std::filesystem::current_path(scratch.path());
    require(find_vendor_font_dir().empty(), "absent default vendor directory must not be found");

    const auto automatic = scratch.path() / "vendor";
    const auto explicit_directory = scratch.path() / "explicit";
    create_font_files(automatic);
    create_font_files(explicit_directory);
    require(find_vendor_font_dir() == automatic, "default discovery must find cwd/vendor");
    require(find_vendor_font_dir(explicit_directory) == explicit_directory,
            "explicit directory must take precedence over cwd/vendor");

    for (const char *missing : {"ASC11", "HZK11"})
    {
        create_font_files(explicit_directory);
        std::filesystem::remove(explicit_directory / missing);
        bool rejected = false;
        try
        {
            find_vendor_font_dir(explicit_directory);
        }
        catch (const std::runtime_error &)
        {
            rejected = true;
        }
        require(rejected, "incomplete explicit directory must not fall back to cwd/vendor");
    }

    bool rejected = false;
    try
    {
        find_vendor_font_dir(scratch.path() / "nonexistent");
    }
    catch (const std::runtime_error &)
    {
        rejected = true;
    }
    require(rejected, "nonexistent explicit directory must not fall back to cwd/vendor");

    std::filesystem::remove(automatic / "HZK11");
    require(find_vendor_font_dir().empty(), "incomplete default vendor directory must not be found");
}
} // namespace

auto main() -> int
{
    try
    {
        test_vendor_font_discovery();
        std::cout << "font discovery tests passed\n";
        return 0;
    }
    catch (const std::exception &error)
    {
        std::cerr << "test failed: " << error.what() << '\n';
        return 1;
    }
}

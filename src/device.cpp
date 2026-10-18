#include "device.hpp"
#include "constants.hpp"

#include <algorithm>
#include <array>
#include <iomanip>
#include <memory>
#include <optional>
#include <ostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace
{
struct DeviceListDeleter
{
    auto operator()(libusb_device **devices) const noexcept -> void
    {
        libusb_free_device_list(devices, 1);
    }
};

struct ConfigDeleter
{
    auto operator()(libusb_config_descriptor *config) const noexcept -> void
    {
        libusb_free_config_descriptor(config);
    }
};

using DeviceList = std::unique_ptr<libusb_device *, DeviceListDeleter>;
using ConfigDescriptor = std::unique_ptr<libusb_config_descriptor, ConfigDeleter>;
using Context = std::shared_ptr<libusb_context>;

class UsbError : public std::runtime_error
{
  public:
    using std::runtime_error::runtime_error;
};

class EndpointError : public std::runtime_error
{
  public:
    using std::runtime_error::runtime_error;
};

class ClaimGuard
{
  public:
    ClaimGuard(libusb_device_handle *handle, int interface_number)
        : handle_(handle), interface_number_(interface_number)
    {
    }

    ~ClaimGuard()
    {
        if (handle_ != nullptr)
        {
            libusb_release_interface(handle_, interface_number_);
        }
    }

    ClaimGuard(const ClaimGuard &) = delete;
    auto operator=(const ClaimGuard &) -> ClaimGuard & = delete;

    auto release() noexcept -> void { handle_ = nullptr; }

  private:
    libusb_device_handle *handle_;
    int interface_number_;
};

auto error_name(int error) -> std::string_view
{
    const char *const name = libusb_error_name(error);
    return name == nullptr ? std::string_view{"unknown libusb error"} : std::string_view{name};
}

[[noreturn]] auto throw_usb_error(std::string_view operation, int error) -> void
{
    throw UsbError(std::string(operation) + ": " + std::string(error_name(error)));
}

auto initialize_context() -> Context
{
    libusb_context *raw_context = nullptr;
    const int init_result = libusb_init(&raw_context);
    if (init_result < 0)
    {
        throw_usb_error("libusb init failed", init_result);
    }
    return Context(raw_context, libusb_exit);
}

struct Enumeration
{
    DeviceList devices;
    ssize_t count{0};
};

auto enumerate_devices(const Context &context) -> Enumeration
{
    libusb_device **raw_devices = nullptr;
    const ssize_t count = libusb_get_device_list(context.get(), &raw_devices);
    if (count < 0)
    {
        throw_usb_error("USB device enumeration failed", static_cast<int>(count));
    }
    return Enumeration{DeviceList(raw_devices), count};
}

auto read_port_path(libusb_device *device) -> std::vector<uint8_t>
{
    std::array<uint8_t, 8> ports{};
    const int count = libusb_get_port_numbers(device, ports.data(), static_cast<int>(ports.size()));
    if (count <= 0)
    {
        return {};
    }
    const auto length = static_cast<std::size_t>(count);
    return {ports.begin(), ports.begin() + static_cast<std::ptrdiff_t>(length)};
}

auto read_configuration(libusb_device *device, bool *active_missing, std::ostream *log = nullptr)
    -> ConfigDescriptor
{
    libusb_config_descriptor *raw_config = nullptr;
    int config_result = libusb_get_active_config_descriptor(device, &raw_config);
    bool missing = false;
    if (config_result == LIBUSB_ERROR_NOT_FOUND)
    {
        missing = true;
        if (log != nullptr)
        {
            *log << "    [config] no active config descriptor, reading index 0\n";
        }
        config_result = libusb_get_config_descriptor(device, 0, &raw_config);
    }
    if (config_result < 0)
    {
        throw_usb_error("failed to read USB configuration", config_result);
    }
    if (active_missing != nullptr)
    {
        *active_missing = missing;
    }
    return ConfigDescriptor(raw_config);
}

auto log_device_descriptor(std::ostream &log, ssize_t index, uint8_t bus, uint8_t address,
                           const libusb_device_descriptor &descriptor) -> void
{
    const auto flags = log.flags();
    const auto fill = log.fill();
    log << "[device " << index << "] bus=" << static_cast<int>(bus)
        << " dev=" << static_cast<int>(address) << " VID:PID=" << std::hex << std::setfill('0')
        << std::setw(4) << descriptor.idVendor << ':' << std::setw(4) << descriptor.idProduct
        << '\n';
    log.flags(flags);
    log.fill(fill);
}

struct EndpointSelection
{
    uint8_t endpoint_out{0};
    uint8_t endpoint_in{0};
    int interface_number{-1};
    int alternate_setting{0};
};

auto find_endpoints(const libusb_config_descriptor &config, std::ostream *log = nullptr)
    -> EndpointSelection
{
    if (log != nullptr)
    {
        *log << "    [endpoint] scanning config with " << static_cast<int>(config.bNumInterfaces)
             << " interface(s)\n";
    }
    for (uint8_t interface_index = 0; interface_index < config.bNumInterfaces; ++interface_index)
    {
        if (log != nullptr)
        {
            *log << "        <scan> interface " << static_cast<int>(interface_index) << "\n";
        }
        const libusb_interface &interface = config.interface[interface_index];
        for (int alternate_index = 0; alternate_index < interface.num_altsetting; ++alternate_index)
        {
            if (log != nullptr)
            {
                *log << "            <scan> altsetting " << alternate_index << "\n";
            }
            const libusb_interface_descriptor &alternate = interface.altsetting[alternate_index];
            EndpointSelection selection{.interface_number = alternate.bInterfaceNumber,
                                        .alternate_setting = alternate.bAlternateSetting};
            for (uint8_t endpoint_index = 0; endpoint_index < alternate.bNumEndpoints;
                 ++endpoint_index)
            {
                const libusb_endpoint_descriptor &endpoint = alternate.endpoint[endpoint_index];
                if ((endpoint.bmAttributes & LIBUSB_TRANSFER_TYPE_MASK) !=
                    LIBUSB_TRANSFER_TYPE_INTERRUPT)
                {
                    continue;
                }
                if ((endpoint.bEndpointAddress & LIBUSB_ENDPOINT_DIR_MASK) == LIBUSB_ENDPOINT_IN)
                {
                    selection.endpoint_in = endpoint.bEndpointAddress;
                }
                else
                {
                    selection.endpoint_out = endpoint.bEndpointAddress;
                }
            }
            if (selection.endpoint_in != 0 && selection.endpoint_out != 0)
            {
                if (log != nullptr)
                {
                    *log << "            [endpoint] selected OUT="
                         << static_cast<int>(selection.endpoint_out)
                         << " IN=" << static_cast<int>(selection.endpoint_in)
                         << " alt=" << selection.alternate_setting << '\n';
                }
                return selection;
            }
        }
    }
    throw EndpointError("device has no usable interrupt IN/OUT endpoint");
}

auto is_target_device(const libusb_device_descriptor &descriptor) -> bool
{
    return descriptor.idVendor == VID && descriptor.idProduct == PID;
}
} // namespace

auto DeviceInfo::device_id() const -> std::string
{
    return std::to_string(bus) + ":" + std::to_string(address) + ":" +
           std::to_string(endpoint_out) + ":" + std::to_string(endpoint_in);
}

auto DeviceInfo::description() const -> std::string
{
    std::ostringstream out;
    out << std::hex << std::setfill('0') << std::setw(4) << VID << ':' << std::setw(4) << PID
        << std::dec << " (bus=" << static_cast<int>(bus)
        << " dev=" << static_cast<int>(address) << " interface=" << interface_number
        << " alternate=" << alternate_setting
        << " endpoint_out=" << static_cast<int>(endpoint_out)
        << " endpoint_in=" << static_cast<int>(endpoint_in) << ')';
    return out.str();
}

auto discover_devices(std::ostream *log) -> std::vector<DeviceInfo>
{
    const Context context = initialize_context();
    if (log != nullptr)
    {
        *log << "[find] libusb initialized\n";
    }
    const Enumeration enumeration = enumerate_devices(context);
    const ssize_t count = enumeration.count;
    if (log != nullptr)
    {
        *log << "[find] enumerated " << count << " USB device(s)\n";
    }

    std::vector<DeviceInfo> result;
    for (ssize_t index = 0; index < count; ++index)
    {
        libusb_device *const device = enumeration.devices.get()[index];
        libusb_device_descriptor descriptor{};
        const int descriptor_result = libusb_get_device_descriptor(device, &descriptor);
        if (descriptor_result < 0)
        {
            if (log != nullptr)
            {
                *log << "[device " << index
                     << "] descriptor read failed: " << error_name(descriptor_result) << '\n';
            }
            continue;
        }
        const uint8_t bus = libusb_get_bus_number(device);
        const uint8_t address = libusb_get_device_address(device);
        if (log != nullptr)
        {
            log_device_descriptor(*log, index, bus, address, descriptor);
        }
        if (!is_target_device(descriptor))
        {
            continue;
        }
        if (log != nullptr)
        {
            *log << "    [match] compatible CH546 display\n";
        }

        DeviceInfo info;
        info.bus = bus;
        info.address = address;
        info.port_path = read_port_path(device);
        if (info.port_path.empty())
        {
            info.discovery_error = "USB port path unavailable; identity cannot be re-verified";
        }

        try
        {
            const ConfigDescriptor config = read_configuration(device, nullptr, log);
            info.configuration_value = config->bConfigurationValue;
            const EndpointSelection endpoints = find_endpoints(*config, log);
            info.endpoint_out = endpoints.endpoint_out;
            info.endpoint_in = endpoints.endpoint_in;
            info.interface_number = endpoints.interface_number;
            info.alternate_setting = endpoints.alternate_setting;
        }
        catch (const UsbError &error)
        {
            if (info.discovery_error.empty())
            {
                info.discovery_error = error.what();
            }
        }
        catch (const EndpointError &error)
        {
            if (info.discovery_error.empty())
            {
                info.discovery_error = error.what();
            }
        }

        if (log != nullptr)
        {
            *log << "    [info] bus=" << static_cast<int>(info.bus)
                 << " dev=" << static_cast<int>(info.address)
                 << " interface=" << info.interface_number
                 << " alternate=" << info.alternate_setting
                 << " out=" << static_cast<int>(info.endpoint_out)
                 << " in=" << static_cast<int>(info.endpoint_in) << '\n';
            if (!info.discovery_error.empty())
            {
                *log << "    [error] " << info.discovery_error << '\n';
            }
        }
        result.push_back(std::move(info));
    }
    if (log != nullptr)
    {
        *log << "[find] found " << result.size() << " compatible display record(s)\n";
    }
    return result;
}

auto select_device(std::span<const DeviceInfo> devices, std::optional<std::string_view> selector)
    -> DeviceInfo
{
    std::string candidates;
    for (const auto &device : devices)
    {
        if (!candidates.empty())
        {
            candidates += ", ";
        }
        candidates += device.device_id();
    }

    const auto with_candidates = [&candidates](std::string message) {
        if (candidates.empty())
        {
            message += " (no compatible 0416:5020 devices reported)";
        }
        else
        {
            message += "; candidates: " + candidates;
        }
        return message;
    };

    if (selector)
    {
        const auto match = std::find_if(devices.begin(), devices.end(),
                                        [&selector](const DeviceInfo &device) {
                                            return std::string_view{device.device_id()} == *selector;
                                        });
        if (match == devices.end())
        {
            throw std::runtime_error(
                with_candidates("device not found: " + std::string(*selector)));
        }
        if (!match->discovery_error.empty())
        {
            throw std::runtime_error(with_candidates("device " + match->device_id() +
                                                     " is unavailable: " + match->discovery_error));
        }
        return *match;
    }

    std::vector<const DeviceInfo *> available;
    for (const auto &device : devices)
    {
        if (device.discovery_error.empty())
        {
            available.push_back(&device);
        }
    }
    if (available.empty())
    {
        throw std::runtime_error(with_candidates(
            "0416:5020 CH546 display not found; check USB connection and udev rules"));
    }
    if (available.size() > 1)
    {
        throw std::runtime_error(
            with_candidates("multiple compatible displays connected; use --device to specify"));
    }
    return *available.front();
}

BadgeDevice::BadgeDevice(std::shared_ptr<libusb_context> context, libusb_device_handle *handle,
                         DeviceInfo info)
    : context_(std::move(context)), handle_(handle), info_(std::move(info))
{
}

BadgeDevice::~BadgeDevice() { close(); }

BadgeDevice::BadgeDevice(BadgeDevice &&other) noexcept { *this = std::move(other); }

auto BadgeDevice::operator=(BadgeDevice &&other) noexcept -> BadgeDevice &
{
    if (this != &other)
    {
        close();
        context_ = std::move(other.context_);
        handle_ = std::exchange(other.handle_, nullptr);
        info_ = std::move(other.info_);
    }
    return *this;
}

auto BadgeDevice::open(const DeviceInfo &device, std::ostream *log) -> BadgeDevice
{
    if (!device.discovery_error.empty())
    {
        throw std::invalid_argument("cannot open USB device " + device.device_id() + ": " +
                                    device.discovery_error);
    }
    if (device.port_path.empty())
    {
        throw std::invalid_argument("cannot open USB device " + device.device_id() +
                                    ": USB port path is empty; refresh the device list and select "
                                    "it again");
    }
    if (device.interface_number < 0 || device.endpoint_out == 0 || device.endpoint_in == 0)
    {
        throw std::invalid_argument("cannot open USB device " + device.device_id() +
                                    ": interface/endpoints are incomplete; refresh the device list "
                                    "and select it again");
    }
    if (device.configuration_value <= 0)
    {
        throw std::invalid_argument("cannot open USB device " + device.device_id() +
                                    ": configuration is incomplete; refresh the device list and "
                                    "select it again");
    }

    const Context context = initialize_context();
    if (log != nullptr)
    {
        *log << "[open] libusb initialized for " << device.device_id() << '\n';
    }
    const Enumeration enumeration = enumerate_devices(context);
    const ssize_t count = enumeration.count;
    if (log != nullptr)
    {
        *log << "[open] enumerated " << count << " USB device(s)\n";
    }

    libusb_device *target = nullptr;
    bool active_config_missing = false;
    bool identity_candidate_seen = false;

    for (ssize_t index = 0; index < count; ++index)
    {
        libusb_device *const candidate = enumeration.devices.get()[index];
        libusb_device_descriptor descriptor{};
        if (libusb_get_device_descriptor(candidate, &descriptor) < 0)
        {
            continue;
        }
        if (!is_target_device(descriptor))
        {
            continue;
        }
        if (libusb_get_bus_number(candidate) != device.bus ||
            libusb_get_device_address(candidate) != device.address ||
            read_port_path(candidate) != device.port_path)
        {
            continue;
        }
        identity_candidate_seen = true;

        bool missing = false;
        ConfigDescriptor config;
        try
        {
            config = read_configuration(candidate, &missing, log);
        }
        catch (const UsbError &error)
        {
            throw std::runtime_error("USB device " + device.device_id() +
                                     " changed identity: " + error.what() +
                                     "; refresh the device list and select it again");
        }
        if (config->bConfigurationValue != device.configuration_value)
        {
            continue;
        }
        try
        {
            const EndpointSelection endpoints = find_endpoints(*config, log);
            if (endpoints.interface_number != device.interface_number ||
                endpoints.endpoint_out != device.endpoint_out ||
                endpoints.endpoint_in != device.endpoint_in ||
                endpoints.alternate_setting != device.alternate_setting)
            {
                continue;
            }
        }
        catch (const EndpointError &)
        {
            continue;
        }
        target = candidate;
        active_config_missing = missing;
        break;
    }

    if (target == nullptr)
    {
        if (identity_candidate_seen)
        {
            throw std::runtime_error("USB device " + device.device_id() +
                                     " changed identity or is unavailable; refresh the device "
                                     "list and select it again");
        }
        throw std::runtime_error("USB device " + device.device_id() +
                                 " not found; refresh the device list and select it again");
    }
    if (log != nullptr)
    {
        *log << "[open] identity verified for " << device.device_id() << '\n';
    }

    libusb_device_handle *raw_handle = nullptr;
    const int open_result = libusb_open(target, &raw_handle);
    if (open_result < 0)
    {
        if (open_result == LIBUSB_ERROR_NO_DEVICE)
        {
            throw std::runtime_error("USB device " + device.device_id() +
                                     " disappeared before it could be opened; refresh the device "
                                     "list and select it again");
        }
        throw_usb_error("failed to open 0416:5020 device (check udev rules)", open_result);
    }
    std::unique_ptr<libusb_device_handle, decltype(&libusb_close)> handle_guard(raw_handle,
                                                                            libusb_close);
    if (log != nullptr)
    {
        *log << "[open] device opened\n";
    }

    if (active_config_missing)
    {
        const int set_result = libusb_set_configuration(raw_handle, device.configuration_value);
        if (set_result < 0 && set_result != LIBUSB_ERROR_BUSY)
        {
            throw_usb_error("failed to set USB configuration", set_result);
        }
        if (set_result == LIBUSB_ERROR_BUSY)
        {
            libusb_config_descriptor *raw_active = nullptr;
            const int active_result = libusb_get_active_config_descriptor(target, &raw_active);
            if (active_result < 0)
            {
                throw std::runtime_error(
                    "USB configuration " + std::to_string(device.configuration_value) +
                    " reported busy and the active configuration cannot be verified (" +
                    std::string(error_name(active_result)) +
                    "); refresh the device list and select it again");
            }
            const ConfigDescriptor active(raw_active);
            if (active->bConfigurationValue != device.configuration_value)
            {
                throw std::runtime_error(
                    "USB configuration " + std::to_string(device.configuration_value) +
                    " reported busy but active configuration " +
                    std::to_string(active->bConfigurationValue) +
                    " differs; refresh the device list and select it again");
            }
            if (log != nullptr)
            {
                *log << "[config] configuration " << device.configuration_value
                     << " already active (BUSY confirmed)\n";
            }
        }
        else if (log != nullptr)
        {
            *log << "[config] configuration " << device.configuration_value << " selected\n";
        }
    }
    else if (log != nullptr)
    {
        *log << "[config] active configuration " << device.configuration_value << " confirmed\n";
    }

    const int detach_result = libusb_set_auto_detach_kernel_driver(raw_handle, 1);
    if (detach_result < 0 && detach_result != LIBUSB_ERROR_NOT_SUPPORTED)
    {
        throw_usb_error("failed to set kernel driver auto-detach", detach_result);
    }

    const int claim_result = libusb_claim_interface(raw_handle, device.interface_number);
    if (claim_result < 0)
    {
        throw_usb_error("failed to claim USB interface", claim_result);
    }
    ClaimGuard claim_guard(raw_handle, device.interface_number);
    if (log != nullptr)
    {
        *log << "[claim] interface " << device.interface_number << " claimed\n";
    }

    if (device.alternate_setting != 0)
    {
        const int alternate_result =
            libusb_set_interface_alt_setting(raw_handle, device.interface_number,
                                             device.alternate_setting);
        if (alternate_result < 0)
        {
            throw_usb_error("failed to select USB alternate setting", alternate_result);
        }
        if (log != nullptr)
        {
            *log << "[alt] alternate setting " << device.alternate_setting << " selected\n";
        }
    }

    BadgeDevice result(context, raw_handle, device);
    static_cast<void>(handle_guard.release());
    claim_guard.release();
    if (log != nullptr)
    {
        *log << "[open] ready\n";
    }
    return result;
}

auto BadgeDevice::device_id() const -> std::string { return info_.device_id(); }

auto BadgeDevice::description() const -> std::string { return info_.description(); }

auto BadgeDevice::write(std::span<const uint8_t> payload, std::chrono::milliseconds delay,
                        unsigned int timeout, std::ostream *log, WriteProgressSink progress) -> void
{
    if (handle_ == nullptr)
    {
        throw std::logic_error("USB device already closed");
    }
    if (payload.empty() || payload.size() % static_cast<std::size_t>(REPORT_SIZE) != 0)
    {
        throw std::invalid_argument("payload length must be a non-zero multiple of 64");
    }
    if (payload.size() > static_cast<std::size_t>(MAX_PAYLOAD))
    {
        throw std::invalid_argument("payload exceeds device safety limit");
    }
    if (delay.count() < 0)
    {
        throw std::invalid_argument("USB report interval cannot be negative");
    }

    const std::size_t reports_total = payload.size() / static_cast<std::size_t>(REPORT_SIZE);
    if (progress.notify != nullptr)
    {
        progress.notify(progress.context, WriteProgress{.reports_completed = 0,
                                                        .reports_total = reports_total});
    }

    if (log != nullptr)
    {
        *log << "[write] " << payload.size() << " bytes, " << reports_total
             << " reports, interval=" << delay.count() << "ms, timeout=" << timeout << "ms\n";
    }

    std::size_t reports_completed = 0;
    for (std::size_t offset = 0; offset < payload.size(); offset += REPORT_SIZE)
    {
        std::this_thread::sleep_for(delay);
        std::array<unsigned char, REPORT_SIZE> report{};
        std::ranges::copy(payload.subspan(offset, REPORT_SIZE), report.begin());
        if (log != nullptr)
        {
            *log << "[write] packet " << ((offset / REPORT_SIZE) + 1) << "/" << reports_total
                 << ": sending OUT\n";
        }
        int written = 0;
        const int write_result = libusb_interrupt_transfer(handle_, info_.endpoint_out,
                                                           report.data(), REPORT_SIZE, &written,
                                                           timeout);
        if (log != nullptr)
        {
            *log << "    OUT " << written << "/" << REPORT_SIZE
                 << " bytes: " << error_name(write_result) << '\n';
        }
        if (write_result < 0)
        {
            throw_usb_error("USB write failed", write_result);
        }
        if (written != REPORT_SIZE)
        {
            throw std::runtime_error("USB wrote only " + std::to_string(written) + "/" +
                                     std::to_string(REPORT_SIZE) + " bytes");
        }
        if (log != nullptr)
        {
            *log << "    waiting for CH546 ACK\n";
        }
        std::array<unsigned char, REPORT_SIZE> acknowledgement{};
        int received = 0;
        const int read_result =
            libusb_interrupt_transfer(handle_, info_.endpoint_in, acknowledgement.data(),
                                      REPORT_SIZE, &received, timeout);
        if (log != nullptr)
        {
            *log << "    ACK " << received << "/" << REPORT_SIZE
                 << " bytes: " << error_name(read_result) << '\n';
        }
        if (read_result < 0)
        {
            throw_usb_error("failed to read CH546 acknowledgement", read_result);
        }
        if (received != REPORT_SIZE)
        {
            throw std::runtime_error("CH546 acknowledgement length mismatch: " +
                                     std::to_string(received) + "/" +
                                     std::to_string(REPORT_SIZE));
        }
        ++reports_completed;
        if (progress.notify != nullptr)
        {
            progress.notify(progress.context, WriteProgress{.reports_completed = reports_completed,
                                                            .reports_total = reports_total});
        }
    }
}

auto BadgeDevice::close() noexcept -> void
{
    if (handle_ != nullptr)
    {
        if (info_.interface_number >= 0)
        {
            libusb_release_interface(handle_, info_.interface_number);
        }
        libusb_close(handle_);
        handle_ = nullptr;
    }
    context_.reset();
}

#include "device.hpp"
#include "constants.hpp"

#include <algorithm>
#include <array>
#include <iomanip>
#include <ostream>
#include <stdexcept>
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

[[noreturn]] auto throw_usb_error(std::string_view operation, int error) -> void
{
    throw std::runtime_error(std::string(operation) + ": " + libusb_error_name(error));
}

auto read_string(libusb_device_handle *handle, uint8_t descriptor_index) -> std::string
{
    if (descriptor_index == 0)
    {
        return "unknown";
    }
    std::array<unsigned char, 256> buffer{};
    const int length = libusb_get_string_descriptor_ascii(handle, descriptor_index, buffer.data(),
                                                          static_cast<int>(buffer.size()));
    if (length < 0)
    {
        return "unknown";
    }
    return {reinterpret_cast<const char *>(buffer.data()), static_cast<std::size_t>(length)};
}

struct EndpointSelection
{
    uint8_t endpoint_out{0};
    uint8_t endpoint_in{0};
    int interface_number{-1};
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
            EndpointSelection selection{.interface_number = alternate.bInterfaceNumber};
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
                         << " IN=" << static_cast<int>(selection.endpoint_in) << '\n';
                }
                return selection;
            }
        }
    }
    throw std::runtime_error("device has no usable interrupt IN/OUT endpoint");
}
} // namespace

BadgeDevice::BadgeDevice(std::shared_ptr<libusb_context> context, libusb_device_handle *handle,
                         uint8_t endpoint_out, uint8_t endpoint_in, int interface_number,
                         uint8_t bus, uint8_t address, std::string manufacturer,
                         std::string product)
    : context_(std::move(context)), handle_(handle), endpoint_out_(endpoint_out),
      endpoint_in_(endpoint_in), interface_number_(interface_number), bus_(bus), address_(address),
      manufacturer_(std::move(manufacturer)), product_(std::move(product))
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
        endpoint_out_ = std::exchange(other.endpoint_out_, 0);
        endpoint_in_ = std::exchange(other.endpoint_in_, 0);
        interface_number_ = std::exchange(other.interface_number_, -1);
        bus_ = std::exchange(other.bus_, 0);
        address_ = std::exchange(other.address_, 0);
        manufacturer_ = std::move(other.manufacturer_);
        product_ = std::move(other.product_);
    }
    return *this;
}

auto BadgeDevice::find_all(std::ostream *log) -> std::vector<BadgeDevice>
{
    libusb_context *raw_context = nullptr;
    const int init_result = libusb_init(&raw_context);
    if (init_result < 0)
    {
        throw_usb_error("libusb init failed", init_result);
    }
    if (log != nullptr)
    {
        *log << "[find] libusb initialized\n";
    }
    auto context = std::shared_ptr<libusb_context>(raw_context, libusb_exit);

    libusb_device **raw_devices = nullptr;
    const ssize_t count = libusb_get_device_list(context.get(), &raw_devices);
    if (count < 0)
    {
        throw_usb_error("USB device enumeration failed", static_cast<int>(count));
    }
    if (log != nullptr)
    {
        *log << "[find] enumerated " << count << " USB device(s)\n";
    }
    DeviceList devices(raw_devices);
    std::vector<BadgeDevice> result;

    for (ssize_t index = 0; index < count; ++index)
    {
        libusb_device *device = devices.get()[index];
        libusb_device_descriptor descriptor{};
        const int descriptor_result = libusb_get_device_descriptor(device, &descriptor);
        if (descriptor_result < 0)
        {
            throw_usb_error("failed to read USB device descriptor", descriptor_result);
        }
        if (log != nullptr)
        {
            const auto flags = log->flags();
            const auto fill = log->fill();
            *log << "[device " << index << "] bus="
                 << static_cast<int>(libusb_get_bus_number(device))
                 << " dev=" << static_cast<int>(libusb_get_device_address(device))
                 << " VID:PID=" << std::hex << std::setfill('0') << std::setw(4)
                 << descriptor.idVendor << ':' << std::setw(4) << descriptor.idProduct << '\n';
            log->flags(flags);
            log->fill(fill);
        }
        if (descriptor.idVendor != VID || descriptor.idProduct != PID)
        {
            continue;
        }
        if (log != nullptr)
        {
            *log << "    [match] compatible CH546 display; opening device\n";
        }

        libusb_device_handle *handle = nullptr;
        const int open_result = libusb_open(device, &handle);
        if (open_result < 0)
        {
            throw_usb_error("failed to open 0416:5020 device (check udev rules)", open_result);
        }
        if (log != nullptr)
        {
            *log << "    [open] device opened\n";
        }
        std::unique_ptr<libusb_device_handle, decltype(&libusb_close)> handle_guard(handle,
                                                                                    libusb_close);

        libusb_config_descriptor *raw_config = nullptr;
        int config_result = libusb_get_active_config_descriptor(device, &raw_config);
        if (config_result == LIBUSB_ERROR_NOT_FOUND)
        {
            if (log != nullptr)
            {
                *log << "    [config] no active config descriptor, falling back to index 0\n";
            }
            config_result = libusb_get_config_descriptor(device, 0, &raw_config);
            if (config_result >= 0)
            {
                const int set_result =
                    libusb_set_configuration(handle, raw_config->bConfigurationValue);
                if (set_result < 0 && set_result != LIBUSB_ERROR_BUSY)
                {
                    libusb_free_config_descriptor(raw_config);
                    throw_usb_error("failed to set USB configuration", set_result);
                }
                if (log != nullptr)
                {
                    *log << "    [config] configuration "
                         << static_cast<int>(raw_config->bConfigurationValue) << ": "
                         << libusb_error_name(set_result) << '\n';
                }
            }
        }
        if (config_result < 0)
        {
            throw_usb_error("failed to read USB configuration", config_result);
        }
        ConfigDescriptor config(raw_config);
        const EndpointSelection endpoints = find_endpoints(*config, log);

        const int detach_result = libusb_set_auto_detach_kernel_driver(handle, 1);
        if (detach_result < 0 && detach_result != LIBUSB_ERROR_NOT_SUPPORTED)
        {
            throw_usb_error("failed to set kernel driver auto-detach", detach_result);
        }
        const int claim_result = libusb_claim_interface(handle, endpoints.interface_number);
        if (claim_result < 0)
        {
            throw_usb_error("failed to claim USB interface", claim_result);
        }
        if (log != nullptr)
        {
            *log << "    [claim] interface " << endpoints.interface_number << " claimed\n";
        }

        std::string manufacturer = read_string(handle, descriptor.iManufacturer);
        std::string product = read_string(handle, descriptor.iProduct);
        if (log != nullptr)
        {
            const auto bus = libusb_get_bus_number(device);
            const auto address = libusb_get_device_address(device);
            *log << "    [info] bus=" << static_cast<int>(bus)
                 << " dev=" << static_cast<int>(address)
                 << " out=" << static_cast<int>(endpoints.endpoint_out)
                 << " in=" << static_cast<int>(endpoints.endpoint_in) << " manufacturer=\""
                 << manufacturer << "\" product=\"" << product << "\"\n";
        }
        result.push_back(BadgeDevice(
            context, handle_guard.release(), endpoints.endpoint_out, endpoints.endpoint_in,
            endpoints.interface_number, libusb_get_bus_number(device),
            libusb_get_device_address(device), std::move(manufacturer), std::move(product)));
    }
    if (log != nullptr)
    {
        *log << "[find] found " << result.size() << " compatible display(s)\n";
    }
    return result;
}

auto BadgeDevice::device_id() const -> std::string
{
    return std::to_string(bus_) + ":" + std::to_string(address_) + ":" +
           std::to_string(endpoint_out_) + ":" + std::to_string(endpoint_in_);
}

auto BadgeDevice::description() const -> std::string
{
    return manufacturer_ + " - " + product_ + " (bus=" + std::to_string(bus_) +
           " dev=" + std::to_string(address_) + " endpoint_out=" + std::to_string(endpoint_out_) +
           " endpoint_in=" + std::to_string(endpoint_in_) + ")";
}

auto BadgeDevice::write(std::span<const uint8_t> payload, std::chrono::milliseconds delay,
                        unsigned int timeout, std::ostream *log) -> void
{
    if (handle_ == nullptr)
    {
        throw std::logic_error("USB device already closed");
    }
    if (payload.empty() || payload.size() % REPORT_SIZE != 0)
    {
        throw std::invalid_argument("payload length must be a non-zero multiple of 64");
    }
    if (payload.size() > MAX_PAYLOAD)
    {
        throw std::invalid_argument("payload exceeds device safety limit");
    }
    if (delay.count() < 0)
    {
        throw std::invalid_argument("USB report interval cannot be negative");
    }

    if (log != nullptr)
    {
        *log << "[write] " << payload.size() << " bytes, " << payload.size() / REPORT_SIZE
             << " reports, interval=" << delay.count() << "ms, timeout=" << timeout << "ms\n";
    }

    for (std::size_t offset = 0; offset < payload.size(); offset += REPORT_SIZE)
    {
        std::this_thread::sleep_for(delay);
        std::array<unsigned char, REPORT_SIZE> report{};
        std::ranges::copy(payload.subspan(offset, REPORT_SIZE), report.begin());
        if (log != nullptr)
        {
            *log << "[write] packet " << ((offset / REPORT_SIZE) + 1) << "/"
                 << (payload.size() / REPORT_SIZE) << ": sending OUT\n";
        }
        int written = 0;
        const int write_result = libusb_interrupt_transfer(handle_, endpoint_out_, report.data(),
                                                           REPORT_SIZE, &written, timeout);
        if (log != nullptr)
        {
            *log << "    OUT " << written << "/" << REPORT_SIZE
                 << " bytes: " << libusb_error_name(write_result) << '\n';
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
        const int read_result = libusb_interrupt_transfer(
            handle_, endpoint_in_, acknowledgement.data(), REPORT_SIZE, &received, timeout);
        if (log != nullptr)
        {
            *log << "    ACK " << received << "/" << REPORT_SIZE
                 << " bytes: " << libusb_error_name(read_result) << '\n';
        }
        if (read_result < 0)
        {
            throw_usb_error("failed to read CH546 acknowledgement", read_result);
        }
        if (received != REPORT_SIZE)
        {
            throw std::runtime_error("CH546 acknowledgement length mismatch: " +
                                     std::to_string(received) + "/" + std::to_string(REPORT_SIZE));
        }
    }
}

auto BadgeDevice::close() noexcept -> void
{
    if (handle_ != nullptr)
    {
        if (interface_number_ >= 0)
        {
            libusb_release_interface(handle_, interface_number_);
        }
        libusb_close(handle_);
        handle_ = nullptr;
    }
    context_.reset();
}

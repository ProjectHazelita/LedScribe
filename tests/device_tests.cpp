// Device lifecycle regressions. The real src/device.cpp is linked into this
// executable while every external libusb entry point is replaced by the fake
// C boundary below. No hardware and no Qt are involved.
#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <ostream>
#include <span>
#include <sstream>
#include <stdexcept>
#include <streambuf>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "constants.hpp"
#include "device.hpp"

#include <libusb.h>

using namespace std::chrono_literals;

struct FakeWorld;
FakeWorld *g_world_pointer = nullptr;

struct FakeEndpoint
{
    uint8_t address{};
    uint8_t attributes{LIBUSB_TRANSFER_TYPE_INTERRUPT};
    uint16_t packet_size{REPORT_SIZE};
    uint8_t interval{1};
};

struct FakeAlternate
{
    int interface_number{};
    int alternate_setting{};
    std::vector<FakeEndpoint> endpoints;
};

struct FakeInterface
{
    std::vector<FakeAlternate> alternates;
};

struct FakeConfig
{
    uint8_t value{1};
    std::vector<FakeInterface> interfaces;

    static auto with_interrupt_pair(uint8_t configuration = 1, uint8_t endpoint_out = 0x01,
                                    uint8_t endpoint_in = 0x81, int interface_number = 0,
                                    int alternate_setting = 0) -> FakeConfig
    {
        FakeAlternate alternate;
        alternate.interface_number = interface_number;
        alternate.alternate_setting = alternate_setting;
        alternate.endpoints.push_back(
            FakeEndpoint{.address = endpoint_out, .attributes = LIBUSB_TRANSFER_TYPE_INTERRUPT,
                         .packet_size = REPORT_SIZE, .interval = 1});
        alternate.endpoints.push_back(
            FakeEndpoint{.address = endpoint_in, .attributes = LIBUSB_TRANSFER_TYPE_INTERRUPT,
                         .packet_size = REPORT_SIZE, .interval = 1});
        FakeInterface interface;
        interface.alternates.push_back(std::move(alternate));
        FakeConfig config;
        config.value = configuration;
        config.interfaces.push_back(std::move(interface));
        return config;
    }
};

struct FakeDevice
{
    libusb_device_descriptor descriptor{};
    uint8_t bus{1};
    uint8_t address{1};
    std::vector<uint8_t> port_path{1, 2};
    int port_numbers_result{LIBUSB_SUCCESS};
    FakeConfig active_config{FakeConfig::with_interrupt_pair()};
    FakeConfig configuration_zero{FakeConfig::with_interrupt_pair()};
    bool has_active_config{true};
    int active_config_result{LIBUSB_SUCCESS};
    int configuration_zero_result{LIBUSB_SUCCESS};
    int open_result{LIBUSB_SUCCESS};
    int descriptor_result{LIBUSB_SUCCESS};
    int claim_result{LIBUSB_SUCCESS};
    int release_result{LIBUSB_SUCCESS};
    int set_configuration_result{LIBUSB_SUCCESS};
    int detach_result{LIBUSB_SUCCESS};
    int alternate_result{LIBUSB_SUCCESS};
    int get_configuration_result{LIBUSB_SUCCESS};
    int reported_configuration{1};
    bool expose_active_config_after_busy{};
    uint8_t active_config_after_busy{1};

    int open_calls{};
    int close_calls{};
    int claim_calls{};
    int release_calls{};
    int set_configuration_calls{};
    int active_config_calls{};
    int get_configuration_calls{};
    int alternate_calls{};
    int detach_calls{};
};

struct FakeHandle
{
    FakeDevice *device{};
    bool claimed{};
    int interface_number{-1};
};

struct OwnedConfig
{
    libusb_config_descriptor descriptor{};
    std::vector<libusb_interface> interfaces;
    std::vector<std::vector<libusb_interface_descriptor>> alternates;
    std::vector<std::vector<std::vector<libusb_endpoint_descriptor>>> endpoints;

    explicit OwnedConfig(const FakeConfig &source)
    {
        interfaces.resize(source.interfaces.size());
        alternates.resize(source.interfaces.size());
        endpoints.resize(source.interfaces.size());
        for (std::size_t interface_index = 0; interface_index < source.interfaces.size();
             ++interface_index)
        {
            const FakeInterface &source_interface = source.interfaces[interface_index];
            alternates[interface_index].resize(source_interface.alternates.size());
            endpoints[interface_index].resize(source_interface.alternates.size());
            for (std::size_t alternate_index = 0;
                 alternate_index < source_interface.alternates.size(); ++alternate_index)
            {
                const FakeAlternate &source_alternate =
                    source_interface.alternates[alternate_index];
                auto &owned_alternate = alternates[interface_index][alternate_index];
                owned_alternate.bLength =
                    static_cast<uint8_t>(sizeof(libusb_interface_descriptor));
                owned_alternate.bDescriptorType =
                    static_cast<uint8_t>(LIBUSB_DT_INTERFACE);
                owned_alternate.bInterfaceNumber =
                    static_cast<uint8_t>(source_alternate.interface_number);
                owned_alternate.bAlternateSetting =
                    static_cast<uint8_t>(source_alternate.alternate_setting);
                owned_alternate.bNumEndpoints =
                    static_cast<uint8_t>(source_alternate.endpoints.size());

                auto &owned_endpoints = endpoints[interface_index][alternate_index];
                owned_endpoints.resize(source_alternate.endpoints.size());
                for (std::size_t endpoint_index = 0;
                     endpoint_index < source_alternate.endpoints.size(); ++endpoint_index)
                {
                    const FakeEndpoint &source_endpoint = source_alternate.endpoints[endpoint_index];
                    auto &owned_endpoint = owned_endpoints[endpoint_index];
                    owned_endpoint.bLength =
                        static_cast<uint8_t>(sizeof(libusb_endpoint_descriptor));
                    owned_endpoint.bDescriptorType =
                        static_cast<uint8_t>(LIBUSB_DT_ENDPOINT);
                    owned_endpoint.bEndpointAddress = source_endpoint.address;
                    owned_endpoint.bmAttributes = source_endpoint.attributes;
                    owned_endpoint.wMaxPacketSize = source_endpoint.packet_size;
                    owned_endpoint.bInterval = source_endpoint.interval;
                }
                owned_alternate.endpoint = owned_endpoints.data();
            }
            interfaces[interface_index].altsetting = alternates[interface_index].data();
            interfaces[interface_index].num_altsetting =
                static_cast<int>(alternates[interface_index].size());
        }
        descriptor.bLength = static_cast<uint8_t>(sizeof(libusb_config_descriptor));
        descriptor.bDescriptorType = static_cast<uint8_t>(LIBUSB_DT_CONFIG);
        descriptor.bNumInterfaces = static_cast<uint8_t>(interfaces.size());
        descriptor.bConfigurationValue = source.value;
        descriptor.interface = interfaces.data();
        descriptor.extra = nullptr;
        descriptor.extra_length = 0;
    }
};

struct TransferScript
{
    int out_error_at{-1};
    int out_short_at{-1};
    int ack_error_at{-1};
    int ack_short_at{-1};
};

struct FakeWorld
{
    std::deque<FakeDevice> devices;
    std::map<libusb_config_descriptor *, std::unique_ptr<OwnedConfig>> owned_configs;
    std::vector<std::string> events;
    std::vector<std::string> calls;
    std::vector<bool> out_phase_visible;
    std::vector<bool> ack_phase_visible;
    TransferScript transfers;

    int init_calls{};
    int init_result{LIBUSB_SUCCESS};
    int exit_calls{};
    int device_list_calls{};
    int device_list_result{LIBUSB_SUCCESS};
    int free_device_list_calls{};
    int string_calls{};
    int open_calls{};
    int close_calls{};
    int claim_calls{};
    int release_calls{};
    int set_configuration_calls{};
    int get_configuration_calls{};
    int alternate_calls{};
    int detach_calls{};
    int transfer_calls{};
    int out_attempts{};
    int ack_attempts{};

    std::ostringstream *log_probe{nullptr};

    FakeWorld() { g_world_pointer = this; }
    ~FakeWorld() { g_world_pointer = nullptr; }
    FakeWorld(const FakeWorld &) = delete;
    auto operator=(const FakeWorld &) -> FakeWorld & = delete;

    auto add_badge(uint8_t bus, uint8_t address, uint8_t endpoint_out = 0x01,
                   uint8_t endpoint_in = 0x81, int interface_number = 0,
                   int alternate_setting = 0, uint8_t configuration = 1) -> FakeDevice &
    {
        FakeDevice device;
        device.descriptor.bLength = static_cast<uint8_t>(sizeof(libusb_device_descriptor));
        device.descriptor.bDescriptorType = static_cast<uint8_t>(LIBUSB_DT_DEVICE);
        device.descriptor.idVendor = VID;
        device.descriptor.idProduct = PID;
        device.bus = bus;
        device.address = address;
        device.active_config = FakeConfig::with_interrupt_pair(
            configuration, endpoint_out, endpoint_in, interface_number, alternate_setting);
        device.configuration_zero = device.active_config;
        device.reported_configuration = configuration;
        devices.push_back(std::move(device));
        return devices.back();
    }

    auto add_other(uint16_t vendor, uint16_t product) -> FakeDevice &
    {
        FakeDevice device;
        device.descriptor.bLength = static_cast<uint8_t>(sizeof(libusb_device_descriptor));
        device.descriptor.bDescriptorType = static_cast<uint8_t>(LIBUSB_DT_DEVICE);
        device.descriptor.idVendor = vendor;
        device.descriptor.idProduct = product;
        device.active_config = FakeConfig::with_interrupt_pair();
        device.configuration_zero = device.active_config;
        devices.push_back(std::move(device));
        return devices.back();
    }
};

struct libusb_context
{
    int marker{};
};

struct libusb_device
{
    FakeDevice *fake{};
};

struct libusb_device_handle
{
    FakeHandle *state{};
};

// ── fake libusb C boundary ────────────────────────────────────────────────
extern "C"
{
    auto LIBUSB_CALL libusb_init(libusb_context **context) -> int
    {
        FakeWorld *world = g_world_pointer;
        ++world->init_calls;
        world->calls.push_back("init");
        if (world->init_result < 0)
        {
            return world->init_result;
        }
        if (context != nullptr)
        {
            *context = new libusb_context{};
        }
        return LIBUSB_SUCCESS;
    }

    auto LIBUSB_CALL libusb_init_context(libusb_context **context,
                                         const libusb_init_option /*options*/[], int /*count*/)
        -> int
    {
        return libusb_init(context);
    }

    void LIBUSB_CALL libusb_exit(libusb_context *context)
    {
        if (context == nullptr)
        {
            return;
        }
        FakeWorld *world = g_world_pointer;
        ++world->exit_calls;
        world->calls.push_back("exit");
        delete context;
    }

    auto LIBUSB_CALL libusb_error_name(int error_code) -> const char *
    {
        switch (error_code)
        {
        case LIBUSB_SUCCESS:
            return "LIBUSB_SUCCESS";
        case LIBUSB_ERROR_IO:
            return "LIBUSB_ERROR_IO";
        case LIBUSB_ERROR_INVALID_PARAM:
            return "LIBUSB_ERROR_INVALID_PARAM";
        case LIBUSB_ERROR_ACCESS:
            return "LIBUSB_ERROR_ACCESS";
        case LIBUSB_ERROR_NO_DEVICE:
            return "LIBUSB_ERROR_NO_DEVICE";
        case LIBUSB_ERROR_NOT_FOUND:
            return "LIBUSB_ERROR_NOT_FOUND";
        case LIBUSB_ERROR_BUSY:
            return "LIBUSB_ERROR_BUSY";
        case LIBUSB_ERROR_TIMEOUT:
            return "LIBUSB_ERROR_TIMEOUT";
        case LIBUSB_ERROR_OVERFLOW:
            return "LIBUSB_ERROR_OVERFLOW";
        case LIBUSB_ERROR_PIPE:
            return "LIBUSB_ERROR_PIPE";
        case LIBUSB_ERROR_INTERRUPTED:
            return "LIBUSB_ERROR_INTERRUPTED";
        case LIBUSB_ERROR_NO_MEM:
            return "LIBUSB_ERROR_NO_MEM";
        case LIBUSB_ERROR_NOT_SUPPORTED:
            return "LIBUSB_ERROR_NOT_SUPPORTED";
        default:
            return "LIBUSB_ERROR_OTHER";
        }
    }

    auto LIBUSB_CALL libusb_get_device_list(libusb_context * /*context*/, libusb_device ***list)
        -> ssize_t
    {
        FakeWorld *world = g_world_pointer;
        ++world->device_list_calls;
        world->calls.push_back("list");
        if (world->device_list_result < 0)
        {
            return world->device_list_result;
        }
        const std::size_t count = world->devices.size();
        auto **devices = new libusb_device *[count + 1]();
        for (std::size_t index = 0; index < count; ++index)
        {
            devices[index] = new libusb_device{&world->devices[index]};
        }
        devices[count] = nullptr;
        *list = devices;
        return static_cast<ssize_t>(count);
    }

    void LIBUSB_CALL libusb_free_device_list(libusb_device **list, int /*unref*/)
    {
        if (list == nullptr)
        {
            return;
        }
        FakeWorld *world = g_world_pointer;
        ++world->free_device_list_calls;
        world->calls.push_back("free-list");
        for (std::size_t index = 0; list[index] != nullptr; ++index)
        {
            delete list[index];
        }
        delete[] list;
    }

    auto LIBUSB_CALL libusb_get_device_descriptor(libusb_device *device,
                                                  libusb_device_descriptor *descriptor) -> int
    {
        g_world_pointer->calls.push_back("descriptor");
        if (device->fake->descriptor_result < 0)
        {
            return device->fake->descriptor_result;
        }
        *descriptor = device->fake->descriptor;
        return LIBUSB_SUCCESS;
    }

    auto LIBUSB_CALL libusb_get_bus_number(libusb_device *device) -> uint8_t
    {
        g_world_pointer->calls.push_back("bus");
        return device->fake->bus;
    }

    auto LIBUSB_CALL libusb_get_device_address(libusb_device *device) -> uint8_t
    {
        g_world_pointer->calls.push_back("address");
        return device->fake->address;
    }

    auto LIBUSB_CALL libusb_get_port_number(libusb_device *device) -> uint8_t
    {
        g_world_pointer->calls.push_back("port-number");
        const auto &path = device->fake->port_path;
        return path.empty() ? 0 : path.back();
    }

    auto LIBUSB_CALL libusb_get_port_numbers(libusb_device *device, uint8_t *path,
                                              int path_length) -> int
    {
        const FakeDevice &fake = *device->fake;
        g_world_pointer->calls.push_back("ports");
        if (fake.port_numbers_result < 0)
        {
            return fake.port_numbers_result;
        }
        if (path_length < 0)
        {
            return LIBUSB_ERROR_INVALID_PARAM;
        }
        const auto available = static_cast<int>(fake.port_path.size());
        const int copied = std::min(available, path_length);
        std::copy_n(fake.port_path.begin(), copied, path);
        return copied;
    }

    auto LIBUSB_CALL libusb_get_string_descriptor_ascii(libusb_device_handle * /*handle*/,
                                                         uint8_t /*index*/,
                                                         unsigned char * /*data*/,
                                                         int /*length*/) -> int
    {
        ++g_world_pointer->string_calls;
        return LIBUSB_ERROR_NOT_FOUND;
    }

    auto LIBUSB_CALL libusb_get_device_string(libusb_device * /*device*/,
                                               libusb_device_string_type /*type*/,
                                               char * /*buffer*/, int /*length*/) -> int
    {
        ++g_world_pointer->string_calls;
        return LIBUSB_ERROR_NOT_FOUND;
    }

    auto LIBUSB_CALL libusb_get_active_config_descriptor(libusb_device *device,
                                                          libusb_config_descriptor **config)
        -> int
    {
        FakeDevice &fake = *device->fake;
        ++fake.active_config_calls;
        g_world_pointer->calls.push_back("active-config");
        if (fake.active_config_result < 0)
        {
            return fake.active_config_result;
        }
        if (!fake.has_active_config)
        {
            return LIBUSB_ERROR_NOT_FOUND;
        }
        auto owned = std::make_unique<OwnedConfig>(fake.active_config);
        libusb_config_descriptor *result = &owned->descriptor;
        g_world_pointer->owned_configs.emplace(result, std::move(owned));
        *config = result;
        return LIBUSB_SUCCESS;
    }

    auto LIBUSB_CALL libusb_get_config_descriptor(libusb_device *device, uint8_t index,
                                                   libusb_config_descriptor **config) -> int
    {
        FakeDevice &fake = *device->fake;
        g_world_pointer->calls.push_back("config0");
        if (fake.configuration_zero_result < 0)
        {
            return fake.configuration_zero_result;
        }
        if (index != 0)
        {
            return LIBUSB_ERROR_NOT_FOUND;
        }
        auto owned = std::make_unique<OwnedConfig>(fake.configuration_zero);
        libusb_config_descriptor *result = &owned->descriptor;
        g_world_pointer->owned_configs.emplace(result, std::move(owned));
        *config = result;
        return LIBUSB_SUCCESS;
    }

    void LIBUSB_CALL libusb_free_config_descriptor(libusb_config_descriptor *config)
    {
        if (config == nullptr)
        {
            return;
        }
        g_world_pointer->calls.push_back("free-config");
        g_world_pointer->owned_configs.erase(config);
    }

    auto LIBUSB_CALL libusb_open(libusb_device *device, libusb_device_handle **handle) -> int
    {
        FakeDevice &fake = *device->fake;
        ++fake.open_calls;
        ++g_world_pointer->open_calls;
        g_world_pointer->calls.push_back("open");
        if (fake.open_result < 0)
        {
            return fake.open_result;
        }
        auto state = std::make_unique<FakeHandle>();
        state->device = &fake;
        *handle = new libusb_device_handle{state.release()};
        return LIBUSB_SUCCESS;
    }

    void LIBUSB_CALL libusb_close(libusb_device_handle *handle)
    {
        if (handle == nullptr)
        {
            return;
        }
        if (handle->state != nullptr)
        {
            ++handle->state->device->close_calls;
            ++g_world_pointer->close_calls;
            g_world_pointer->calls.push_back("close");
            delete handle->state;
        }
        delete handle;
    }

    auto LIBUSB_CALL libusb_claim_interface(libusb_device_handle *handle, int interface_number)
        -> int
    {
        FakeHandle &state = *handle->state;
        ++state.device->claim_calls;
        ++g_world_pointer->claim_calls;
        g_world_pointer->calls.push_back("claim");
        if (state.device->claim_result < 0)
        {
            return state.device->claim_result;
        }
        state.claimed = true;
        state.interface_number = interface_number;
        return LIBUSB_SUCCESS;
    }

    auto LIBUSB_CALL libusb_release_interface(libusb_device_handle *handle, int interface_number)
        -> int
    {
        FakeHandle &state = *handle->state;
        ++state.device->release_calls;
        ++g_world_pointer->release_calls;
        g_world_pointer->calls.push_back("release");
        if (state.claimed && state.interface_number == interface_number)
        {
            state.claimed = false;
        }
        return state.device->release_result;
    }

    auto LIBUSB_CALL libusb_set_configuration(libusb_device_handle *handle, int configuration)
        -> int
    {
        FakeHandle &state = *handle->state;
        ++state.device->set_configuration_calls;
        ++g_world_pointer->set_configuration_calls;
        g_world_pointer->calls.push_back("set-configuration");
        if (state.device->set_configuration_result == LIBUSB_SUCCESS)
        {
            state.device->reported_configuration = configuration;
        }
        else if (state.device->set_configuration_result == LIBUSB_ERROR_BUSY &&
                 state.device->expose_active_config_after_busy)
        {
            state.device->active_config_result = LIBUSB_SUCCESS;
            state.device->has_active_config = true;
            state.device->active_config = state.device->configuration_zero;
            state.device->active_config.value = state.device->active_config_after_busy;
        }
        return state.device->set_configuration_result;
    }

    auto LIBUSB_CALL libusb_get_configuration(libusb_device_handle *handle,
                                               int *configuration) -> int
    {
        FakeHandle &state = *handle->state;
        ++state.device->get_configuration_calls;
        ++g_world_pointer->get_configuration_calls;
        if (state.device->get_configuration_result < 0)
        {
            return state.device->get_configuration_result;
        }
        *configuration = state.device->reported_configuration;
        return LIBUSB_SUCCESS;
    }

    auto LIBUSB_CALL libusb_set_interface_alt_setting(libusb_device_handle *handle,
                                                       int interface_number,
                                                       int alternate_setting) -> int
    {
        FakeHandle &state = *handle->state;
        ++state.device->alternate_calls;
        ++g_world_pointer->alternate_calls;
        g_world_pointer->calls.push_back("alternate");
        state.device->reported_configuration = alternate_setting;
        (void)interface_number;
        return state.device->alternate_result;
    }

    auto LIBUSB_CALL libusb_set_auto_detach_kernel_driver(libusb_device_handle *handle,
                                                           int /*enable*/) -> int
    {
        FakeHandle &state = *handle->state;
        ++state.device->detach_calls;
        ++g_world_pointer->detach_calls;
        g_world_pointer->calls.push_back("detach");
        return state.device->detach_result;
    }

    auto LIBUSB_CALL libusb_interrupt_transfer(libusb_device_handle *,
                                                unsigned char endpoint, unsigned char *data,
                                                int length, int *transferred, unsigned int timeout)
        -> int
    {
        FakeWorld &world = *g_world_pointer;
        ++world.transfer_calls;
        (void)timeout;

        const bool outbound =
            (endpoint & LIBUSB_ENDPOINT_DIR_MASK) == LIBUSB_ENDPOINT_OUT;
        if (outbound)
        {
            const int attempt = world.out_attempts++;
            world.calls.push_back("OUT");
            if (world.log_probe != nullptr)
            {
                const std::string current = world.log_probe->str();
                world.out_phase_visible.push_back(current.find("sending OUT") != std::string::npos);
            }
            if (world.transfers.out_error_at == attempt)
            {
                *transferred = 0;
                world.events.push_back("OUT" + std::to_string(attempt) + ":error");
                return LIBUSB_ERROR_TIMEOUT;
            }
            if (world.transfers.out_short_at == attempt)
            {
                *transferred = 7;
                if (length > 0)
                {
                    data[0] = 0xA5;
                }
                world.events.push_back("OUT" + std::to_string(attempt) + ":short");
                return LIBUSB_SUCCESS;
            }
            std::fill_n(data, length, static_cast<unsigned char>(0x42 + attempt));
            *transferred = length;
            world.events.push_back("OUT" + std::to_string(attempt) + ":ok");
            return LIBUSB_SUCCESS;
        }

        const int attempt = world.ack_attempts++;
        world.calls.push_back("ACK");
        if (world.log_probe != nullptr)
        {
            const std::string current = world.log_probe->str();
            world.ack_phase_visible.push_back(current.find("waiting for CH546 ACK") !=
                                              std::string::npos);
        }
        if (world.transfers.ack_error_at == attempt)
        {
            *transferred = 0;
            world.events.push_back("ACK" + std::to_string(attempt) + ":error");
            return LIBUSB_ERROR_NO_DEVICE;
        }
        if (world.transfers.ack_short_at == attempt)
        {
            *transferred = 3;
            world.events.push_back("ACK" + std::to_string(attempt) + ":short");
            return LIBUSB_SUCCESS;
        }
        std::fill_n(data, length, static_cast<unsigned char>(0x06));
        *transferred = length;
        world.events.push_back("ACK" + std::to_string(attempt) + ":ok");
        return LIBUSB_SUCCESS;
    }
} // extern "C"

namespace
{
auto require(bool condition, std::string_view message) -> void
{
    if (!condition)
    {
        throw std::runtime_error(std::string(message));
    }
}

auto contains(std::string_view haystack, std::string_view needle) -> bool
{
    return haystack.find(needle) != std::string_view::npos;
}

template <typename Function>
auto require_throws(Function &&function, std::string_view message) -> std::string
{
    try
    {
        std::forward<Function>(function)();
    }
    catch (const std::exception &error)
    {
        return error.what();
    }
    throw std::runtime_error(std::string("expected an exception: ") + std::string(message));
}

auto protocol_payload(std::size_t reports) -> std::vector<uint8_t>
{
    std::vector<uint8_t> payload(reports * REPORT_SIZE);
    for (std::size_t index = 0; index < payload.size(); ++index)
    {
        payload[index] = static_cast<uint8_t>(index);
    }
    return payload;
}

struct ProgressRecorder
{
    std::vector<WriteProgress> entries;
};

auto record_progress(void *context, WriteProgress progress) noexcept -> void
{
    static_cast<ProgressRecorder *>(context)->entries.push_back(progress);
}

auto make_sink(ProgressRecorder &recorder) -> WriteProgressSink
{
    return WriteProgressSink{.context = &recorder, .notify = record_progress};
}

auto discover_only(FakeWorld &world, std::ostream *log = nullptr) -> std::vector<DeviceInfo>
{
    const int before_open = world.open_calls;
    const int before_claim = world.claim_calls;
    const int before_config = world.set_configuration_calls;
    const int before_detach = world.detach_calls;
    const int before_alternate = world.alternate_calls;
    const int before_transfer = world.transfer_calls;
    auto devices = discover_devices(log);
    require(world.open_calls == before_open, "discovery must not open a device");
    require(world.claim_calls == before_claim, "discovery must not claim an interface");
    require(world.set_configuration_calls == before_config,
            "discovery must not set a configuration");
    require(world.detach_calls == before_detach,
            "discovery must not enable kernel driver auto-detach");
    require(world.alternate_calls == before_alternate,
            "discovery must not select an alternate setting");
    require(world.transfer_calls == before_transfer, "discovery must not transfer");
    require(world.string_calls == 0, "discovery must not read opened-handle strings");
    return devices;
}

auto open_selected(const DeviceInfo &info, std::ostream *log = nullptr) -> BadgeDevice
{
    return BadgeDevice::open(info, log);
}

auto run_write(FakeWorld &, const DeviceInfo &info, std::span<const uint8_t> payload,
               ProgressRecorder &progress, std::ostream *log = nullptr) -> std::string
{
    try
    {
        auto device = open_selected(info, log);
        device.write(payload, 0ms, 10, log, make_sink(progress));
        return {};
    }
    catch (const std::exception &error)
    {
        return error.what();
    }
}

auto setup_single_badge(FakeWorld &world) -> DeviceInfo
{
    world.add_badge(1, 7);
    auto devices = discover_only(world);
    require(devices.size() == 1, "expected one discovered badge");
    return devices.front();
}

auto test_discovery_is_descriptor_only() -> void
{
    FakeWorld world;
    world.add_badge(1, 7);
    world.add_other(0x1234, 0x5678);
    const auto devices = discover_only(world);
    require(devices.size() == 1, "only the CH546 badge should be reported");
    require(devices.front().bus == 1, "bus mismatch");
    require(devices.front().address == 7, "address mismatch");
    require(devices.front().endpoint_out == 0x01, "OUT endpoint mismatch");
    require(devices.front().endpoint_in == 0x81, "IN endpoint mismatch");
    require(devices.front().interface_number == 0, "interface mismatch");
    require(devices.front().alternate_setting == 0, "alternate mismatch");
    require(devices.front().configuration_value == 1, "configuration mismatch");
    require(devices.front().port_path == std::vector<uint8_t>({1, 2}), "port path mismatch");
    require(devices.front().discovery_error.empty(), "good device must not have an error");
    require(devices.front().device_id() == "1:7:1:129", "stable decimal device id changed");
    const std::string description = devices.front().description();
    require(contains(description, "0416") || contains(description, "0x0416"),
            "description must identify the vendor id");
    require(contains(description, "5020") || contains(description, "0x5020"),
            "description must identify the product id");
    require(world.init_calls >= 1, "discovery must initialize libusb");
    require(world.init_calls == world.exit_calls, "discovery must balance libusb init/exit");
    require(world.free_device_list_calls >= 1, "discovery must free the device list");
}

auto test_discovery_keeps_good_device_next_to_broken_one() -> void
{
    FakeWorld world;
    FakeDevice &broken = world.add_badge(2, 3);
    broken.configuration_zero_result = LIBUSB_ERROR_IO;
    broken.active_config_result = LIBUSB_ERROR_NOT_FOUND;
    FakeDevice &unreadable = world.add_other(0x9999, 0x0001);
    unreadable.descriptor_result = LIBUSB_ERROR_IO;
    world.add_badge(2, 4);

    auto devices = discover_only(world);
    require(devices.size() == 2, "both matching devices must be reported");
    const DeviceInfo *unavailable = nullptr;
    const DeviceInfo *usable = nullptr;
    for (const auto &device : devices)
    {
        if (device.discovery_error.empty())
        {
            usable = &device;
        }
        else
        {
            unavailable = &device;
        }
    }
    require(unavailable != nullptr && !unavailable->discovery_error.empty(),
            "broken badge must carry a discovery error");
    require(usable != nullptr, "good badge must remain usable");
    const std::string usable_id = usable->device_id();
    const std::string selected =
        select_device(devices, std::optional<std::string_view>{std::string_view{usable_id}})
            .device_id();
    require(selected == usable_id, "explicit usable selection failed");

    const DeviceInfo no_selector =
        select_device(devices);
    require(no_selector.device_id() == usable->device_id(),
            "one usable target must be selected without a selector");
    const std::string broken_error = require_throws(
        [&] {
            const std::string id = unavailable->device_id();
            (void)select_device(devices, std::optional<std::string_view>{id});
        },
        "unavailable target");
    require(contains(broken_error, unavailable->device_id()),
            "unavailable selection error must name the target");
}

auto test_discovery_marks_missing_port_path() -> void
{
    FakeWorld world;
    FakeDevice &fake = world.add_badge(2, 5);
    fake.port_path.clear();
    auto devices = discover_only(world);
    require(devices.size() == 1, "record without a port path must still be listed");
    require(!devices.front().discovery_error.empty(),
            "a missing port path must make the record unavailable");
    require(devices.front().discovery_error.find("port path") != std::string::npos,
            "the error must explain the missing port path");
    require_throws([&] { (void)select_device(devices); }, "unavailable-only selection");
}

auto test_selection_requires_exact_match() -> void
{
    FakeWorld world;
    world.add_badge(3, 10, 0x02, 0x82);
    world.add_badge(3, 11, 0x03, 0x83);
    auto devices = discover_only(world);
    require(devices.size() == 2, "expected two badges");

    const std::string ambiguous = require_throws(
        [&] { (void)select_device(devices); }, "multiple usable targets");
    require(contains(ambiguous, devices[0].device_id()) &&
                contains(ambiguous, devices[1].device_id()),
            "ambiguous error must list every candidate");

    const std::string second_id = devices[1].device_id();
    const DeviceInfo chosen =
        select_device(devices, std::optional<std::string_view>{second_id});
    require(chosen.device_id() == second_id, "selection must match the exact device id");
    require(chosen.endpoint_out == 0x03 && chosen.endpoint_in == 0x83,
            "selection must not fall back to the first device");

    const std::string missing =
        require_throws([&] { (void)select_device(devices, std::optional<std::string_view>{"9:9:9:9"}); },
                       "missing selector");
    require(contains(missing, devices[0].device_id()) &&
                contains(missing, devices[1].device_id()),
            "missing selection error must list candidates");
}

auto test_open_uses_only_selected_device() -> void
{
    FakeWorld world;
    FakeDevice &first = world.add_badge(4, 20);
    FakeDevice &second = world.add_badge(4, 21);
    auto devices = discover_only(world);
    require(devices.size() == 2, "expected two badges");
    const std::string selected_id = devices[1].device_id();
    const DeviceInfo selected =
        select_device(devices, std::optional<std::string_view>{selected_id});

    auto device = BadgeDevice::open(selected);
    require(second.open_calls == 1, "selected device must be opened exactly once");
    require(first.open_calls == 0, "unselected device must not be opened");
    require(second.claim_calls == 1, "selected interface must be claimed");
    require(first.claim_calls == 0, "unselected interface must not be claimed");
    require(second.detach_calls == 1, "selected handle must enable auto-detach");
    require(first.detach_calls == 0, "unselected handle must not be touched");
    require(second.alternate_calls == 0, "alternate setting zero must not issue SET_INTERFACE");
    device.close();
    require(second.release_calls == 1, "close must release the claimed interface");
    require(second.close_calls == 1, "close must close the handle");
    require(world.init_calls == world.exit_calls, "open must balance libusb init/exit");
}

auto test_open_rejects_identity_changes() -> void
{
    {
        FakeWorld world;
        world.add_badge(5, 30);
        auto devices = discover_only(world);
        world.devices.front().address = 31;
        const std::string error = require_throws([&] { (void)open_selected(devices.front()); },
                                                 "address changed");
        require(!error.empty(), "identity error must explain the failure");
        require(world.devices.front().open_calls == 0,
                "a vanished address must not be followed to another device");
    }
    {
        FakeWorld world;
        world.add_badge(5, 32);
        auto devices = discover_only(world);
        world.devices.front().port_path = {1, 3};
        require_throws([&] { (void)open_selected(devices.front()); }, "port path changed");
        require(world.devices.front().open_calls == 0,
                "a changed port path must not be treated as the same device");
    }
    {
        FakeWorld world;
        world.add_badge(5, 33);
        auto devices = discover_only(world);
        world.devices.front().active_config =
            FakeConfig::with_interrupt_pair(1, 0x04, 0x84);
        world.devices.front().configuration_zero = world.devices.front().active_config;
        require_throws([&] { (void)open_selected(devices.front()); }, "endpoints changed");
        require(world.devices.front().claim_calls == 0,
                "changed endpoints must be rejected before claiming");
    }
    {
        FakeWorld world;
        world.add_badge(5, 34);
        auto devices = discover_only(world);
        world.devices.front().active_config.value = 2;
        world.devices.front().configuration_zero.value = 2;
        require_throws([&] { (void)open_selected(devices.front()); }, "configuration changed");
        require(world.devices.front().open_calls == 0,
                "changed configuration must be rejected before opening");
    }
    {
        FakeWorld world;
        world.add_badge(5, 35);
        auto devices = discover_only(world);
        world.devices.front().active_config =
            FakeConfig::with_interrupt_pair(1, 0x01, 0x81, 0, 1);
        world.devices.front().configuration_zero = world.devices.front().active_config;
        require_throws([&] { (void)open_selected(devices.front()); }, "alternate changed");
        require(world.devices.front().open_calls == 0,
                "changed alternate setting must be rejected before opening");
    }
}

auto test_claim_rollback_on_throwing_log() -> void
{
    FakeWorld world;
    world.add_badge(6, 40);

    struct ThrowingBuffer : std::streambuf
    {
        std::string content;

        auto check() -> void
        {
            if (content.find("claim") != std::string::npos)
            {
                throw std::runtime_error("injected log failure");
            }
        }

        auto xsputn(const char *text, std::streamsize length) -> std::streamsize override
        {
            content.append(text, static_cast<std::size_t>(length));
            check();
            return length;
        }

        auto overflow(int character) -> int override
        {
            if (character != std::char_traits<char>::eof())
            {
                content.push_back(static_cast<char>(character));
                check();
            }
            return character;
        }
    } buffer;
    std::ostream throwing_log(&buffer);
    throwing_log.exceptions(std::ios::badbit | std::ios::failbit);

    FakeDevice &fake = world.devices.front();
    const std::string error = require_throws(
        [&] { (void)BadgeDevice::open(discover_only(world).front(), &throwing_log); },
        "log exception after claim");
    require(contains(error, "injected"), "the injected log exception must propagate");
    require(fake.claim_calls == 1, "claim must have happened before the throwing log");
    require(fake.release_calls == 1, "claim guard must release after the exception");
    require(fake.close_calls == 1, "open handle must be closed after the exception");
    require(world.init_calls == world.exit_calls,
            "context must be exited after the exception");
    require(world.calls.size() >= 4 &&
                world.calls[world.calls.size() - 4] == "release" &&
                world.calls[world.calls.size() - 3] == "close" &&
                world.calls[world.calls.size() - 2] == "free-list" &&
                world.calls.back() == "exit",
            "rollback must release, close, free the list, then exit the context");
}

auto test_claim_rollback_on_alternate_failure() -> void
{
    FakeWorld world;
    FakeDevice &fake = world.add_badge(6, 41, 0x01, 0x81, 0, 1);
    auto devices = discover_only(world);
    fake.alternate_result = LIBUSB_ERROR_PIPE;
    require_throws([&] { (void)open_selected(devices.front()); }, "alternate failure");
    require(fake.claim_calls == 1, "alternate failure requires a claimed interface");
    require(fake.alternate_calls == 1, "nonzero alternate must be selected");
    require(fake.release_calls == 1, "alternate failure must release the interface");
    require(fake.close_calls == 1, "alternate failure must close the handle");
    require(world.init_calls == world.exit_calls, "alternate failure must exit the context");
    require(world.calls.size() >= 4 &&
                world.calls[world.calls.size() - 4] == "release" &&
                world.calls[world.calls.size() - 3] == "close" &&
                world.calls[world.calls.size() - 2] == "free-list" &&
                world.calls.back() == "exit",
            "alternate rollback must release, close, free the list, then exit");
}

auto test_alternate_zero_is_not_set() -> void
{
    FakeWorld world;
    FakeDevice &fake = world.add_badge(6, 42);
    auto devices = discover_only(world);
    auto device = open_selected(devices.front());
    require(fake.alternate_calls == 0,
            "alternate setting zero must not issue a redundant SET_INTERFACE");
    device.close();
}

auto test_nonzero_alternate_is_selected() -> void
{
    FakeWorld world;
    FakeDevice &fake = world.add_badge(6, 43, 0x01, 0x81, 0, 1);
    auto devices = discover_only(world);
    require(devices.front().alternate_setting == 1, "discovery must report alternate setting one");
    auto device = open_selected(devices.front());
    require(fake.alternate_calls == 1, "nonzero alternate must call SET_INTERFACE once");
    device.close();
}

auto test_configuration_busy_requires_verified_config() -> void
{
    {
        FakeWorld world;
        FakeDevice &fake = world.add_badge(7, 50);
        fake.has_active_config = false;
        fake.active_config_result = LIBUSB_ERROR_NOT_FOUND;
        fake.configuration_zero.value = 2;
        fake.configuration_zero.interfaces.front().alternates.front().interface_number = 0;
        fake.reported_configuration = 2;
        fake.set_configuration_result = LIBUSB_ERROR_BUSY;
        fake.expose_active_config_after_busy = true;
        fake.active_config_after_busy = 2;
        auto devices = discover_only(world);
        require(devices.front().configuration_value == 2,
                "fallback config zero must be reported");

        auto device = open_selected(devices.front());
        require(fake.set_configuration_calls == 1, "required configuration must be requested");
        require(fake.active_config_calls >= 2,
                "BUSY must be verified by re-reading the active configuration");
        require(fake.claim_calls == 1, "verified BUSY configuration must proceed to claim");
        device.close();
    }
    {
        FakeWorld world;
        FakeDevice &fake = world.add_badge(7, 51);
        fake.has_active_config = false;
        fake.active_config_result = LIBUSB_ERROR_NOT_FOUND;
        fake.configuration_zero.value = 2;
        fake.reported_configuration = 1;
        fake.set_configuration_result = LIBUSB_ERROR_BUSY;
        fake.expose_active_config_after_busy = true;
        fake.active_config_after_busy = 1;
        auto devices = discover_only(world);
        fake.reported_configuration = 1;
        require_throws([&] { (void)open_selected(devices.front()); },
                       "unverified BUSY configuration");
        require(fake.set_configuration_calls == 1, "configuration must be attempted once");
        require(fake.active_config_calls >= 2,
                "BUSY must be checked against the active configuration");
        require(fake.claim_calls == 0, "an unverified BUSY configuration must not be claimed");
        require(fake.close_calls == 1, "failed configuration must close the handle");
    }
}

auto test_write_reports_truthful_progress() -> void
{
    FakeWorld world;
    const DeviceInfo info = setup_single_badge(world);
    const auto payload = protocol_payload(3);
    ProgressRecorder progress;

    auto device = open_selected(info);
    device.write(payload, 0ms, 10, nullptr, make_sink(progress));

    require(progress.entries.size() == 4, "progress must start at zero and follow every ACK");
    require(progress.entries[0].reports_completed == 0 && progress.entries[0].reports_total == 3,
            "initial progress must be 0/3");
    for (std::size_t index = 1; index < progress.entries.size(); ++index)
    {
        require(progress.entries[index].reports_completed == index &&
                    progress.entries[index].reports_total == 3,
                "progress must advance only after a complete ACK");
    }
    require(world.events ==
                std::vector<std::string>{"OUT0:ok", "ACK0:ok", "OUT1:ok", "ACK1:ok", "OUT2:ok",
                                         "ACK2:ok"},
            "transfer order mismatch");
    require(world.transfer_calls == 6, "expected three OUT and three ACK transfers");
}

auto test_out_failure_stops_before_next_report() -> void
{
    FakeWorld world;
    const DeviceInfo info = setup_single_badge(world);
    const auto payload = protocol_payload(3);
    world.transfers.out_error_at = 1;
    ProgressRecorder progress;

    const std::string error = run_write(world, info, payload, progress);
    require(!error.empty(), "OUT failure must throw");
    require(progress.entries.size() == 2, "only the completed report may be reported");
    require(progress.entries.back().reports_completed == 1,
            "failed OUT must not increment completed reports");
    require(world.events == std::vector<std::string>{"OUT0:ok", "ACK0:ok", "OUT1:error"},
            "no later report may be sent after an OUT failure");
}

auto test_short_out_stops_before_ack() -> void
{
    FakeWorld world;
    const DeviceInfo info = setup_single_badge(world);
    const auto payload = protocol_payload(3);
    world.transfers.out_short_at = 1;
    ProgressRecorder progress;

    const std::string error = run_write(world, info, payload, progress);
    require(!error.empty(), "short OUT must throw");
    require(progress.entries.size() == 2, "short OUT must not count as complete");
    require(progress.entries.back().reports_completed == 1,
            "short OUT must not increment completed reports");
    require(world.events == std::vector<std::string>{"OUT0:ok", "ACK0:ok", "OUT1:short"},
            "short OUT must not wait for an ACK or send the next report");
}

auto test_short_ack_stops_before_next_report() -> void
{
    FakeWorld world;
    const DeviceInfo info = setup_single_badge(world);
    const auto payload = protocol_payload(3);
    world.transfers.ack_short_at = 1;
    ProgressRecorder progress;

    const std::string error = run_write(world, info, payload, progress);
    require(!error.empty(), "short ACK must throw");
    require(progress.entries.size() == 2, "only the completed report may be reported");
    require(progress.entries.back().reports_completed == 1,
            "short ACK must not increment completed reports");
    require(world.events ==
                std::vector<std::string>{"OUT0:ok", "ACK0:ok", "OUT1:ok", "ACK1:short"},
            "short ACK must not be followed by another report");
}

struct WriteOutcome
{
    std::string error;
    std::vector<std::string> events;
    std::vector<std::string> calls;
    std::vector<WriteProgress> progress;
    std::vector<bool> out_phase_visible;
    std::vector<bool> ack_phase_visible;
    std::string log;
};

auto run_verbose_scenario(bool verbose) -> WriteOutcome
{
    FakeWorld world;
    world.add_badge(8, 60);
    world.transfers.ack_error_at = 2;
    ProgressRecorder recorder;
    std::ostringstream log;
    if (verbose)
    {
        world.log_probe = &log;
    }
    auto devices = discover_only(world, verbose ? &log : nullptr);
    WriteOutcome outcome;
    outcome.error = run_write(world, devices.front(), protocol_payload(3), recorder,
                              verbose ? &log : nullptr);
    outcome.events = world.events;
    outcome.calls = world.calls;
    outcome.progress = recorder.entries;
    outcome.out_phase_visible = world.out_phase_visible;
    outcome.ack_phase_visible = world.ack_phase_visible;
    outcome.log = log.str();
    return outcome;
}

auto test_verbose_and_silent_are_equivalent() -> void
{
    const WriteOutcome silent = run_verbose_scenario(false);
    const WriteOutcome verbose = run_verbose_scenario(true);
    require(!silent.error.empty(), "scenario must fail");
    require(silent.error == verbose.error, "logging must not change the exception");
    require(silent.events == verbose.events, "logging must not change the transfer sequence");
    require(silent.calls == verbose.calls,
            "logging must not change the libusb call sequence");
    require(silent.progress.size() == verbose.progress.size(), "logging must not change progress");
    for (std::size_t index = 0; index < silent.progress.size(); ++index)
    {
        require(silent.progress[index].reports_completed ==
                        verbose.progress[index].reports_completed &&
                    silent.progress[index].reports_total == verbose.progress[index].reports_total,
                "logging must not change progress values");
    }
    require(!verbose.out_phase_visible.empty() && !verbose.ack_phase_visible.empty(),
            "verbose transfer phases must be recorded");
    require(std::all_of(verbose.out_phase_visible.begin(), verbose.out_phase_visible.end(),
                        [](bool visible) { return visible; }),
            "the OUT phase log must be visible before each blocking transfer");
    require(std::all_of(verbose.ack_phase_visible.begin(), verbose.ack_phase_visible.end(),
                        [](bool visible) { return visible; }),
            "the ACK phase log must be visible before each blocking transfer");
    require(verbose.log.find("[write]") != std::string::npos, "verbose write log missing");
}

auto test_stream_flags_and_fill_are_preserved() -> void
{
    FakeWorld world;
    FakeDevice &fake = world.add_badge(9, 70);
    fake.port_path = {1, 2, 3};
    std::ostringstream log;
    log << std::hex << std::setfill('*');

    auto devices = discover_only(world, &log);
    require(devices.size() == 1, "expected one badge");
    require((log.flags() & std::ios::basefield) == std::ios::hex,
            "discovery must restore the stream base");
    require(log.fill() == '*', "discovery must restore the stream fill");

    auto device = open_selected(devices.front(), &log);
    require((log.flags() & std::ios::basefield) == std::ios::hex,
            "open must restore the stream base");
    require(log.fill() == '*', "open must restore the stream fill");

    ProgressRecorder progress;
    device.write(protocol_payload(1), 0ms, 10, &log, make_sink(progress));
    require((log.flags() & std::ios::basefield) == std::ios::hex,
            "write must restore the stream base");
    require(log.fill() == '*', "write must restore the stream fill");

    const std::size_t before = log.str().size();
    log << 255;
    require(log.str().substr(before) == "ff",
            "preserved hexadecimal formatting must still apply after device logs");
    require(log.fill() == '*', "stream fill must remain untouched");
    device.close();

    FakeWorld plain_world;
    plain_world.add_badge(9, 71);
    std::ostringstream plain;
    const auto plain_devices = discover_only(plain_world, &plain);
    require(plain_devices.size() == 1, "expected one badge for the decimal stream check");
    const std::size_t plain_before = plain.str().size();
    plain << 255;
    require(plain.str().substr(plain_before) == "255",
            "device logs must not leave hexadecimal formatting behind");
    require(plain.fill() == ' ', "device logs must not leave a padding fill behind");
}

auto run_test(std::string_view name, void (*function)()) -> bool
{
    try
    {
        function();
        return true;
    }
    catch (const std::exception &error)
    {
        std::cerr << "FAILED " << name << ": " << error.what() << '\n';
        return false;
    }
}
} // namespace

auto main() -> int
{
    struct TestCase
    {
        std::string_view name;
        void (*function)();
    };
    const std::array tests = {
        TestCase{"discovery_is_descriptor_only", &test_discovery_is_descriptor_only},
        TestCase{"discovery_keeps_good_device_next_to_broken_one",
                 &test_discovery_keeps_good_device_next_to_broken_one},
        TestCase{"discovery_marks_missing_port_path", &test_discovery_marks_missing_port_path},
        TestCase{"selection_requires_exact_match", &test_selection_requires_exact_match},
        TestCase{"open_uses_only_selected_device", &test_open_uses_only_selected_device},
        TestCase{"open_rejects_identity_changes", &test_open_rejects_identity_changes},
        TestCase{"claim_rollback_on_throwing_log", &test_claim_rollback_on_throwing_log},
        TestCase{"claim_rollback_on_alternate_failure", &test_claim_rollback_on_alternate_failure},
        TestCase{"alternate_zero_is_not_set", &test_alternate_zero_is_not_set},
        TestCase{"nonzero_alternate_is_selected", &test_nonzero_alternate_is_selected},
        TestCase{"configuration_busy_requires_verified_config",
                 &test_configuration_busy_requires_verified_config},
        TestCase{"write_reports_truthful_progress", &test_write_reports_truthful_progress},
        TestCase{"out_failure_stops_before_next_report",
                 &test_out_failure_stops_before_next_report},
        TestCase{"short_out_stops_before_ack", &test_short_out_stops_before_ack},
        TestCase{"short_ack_stops_before_next_report", &test_short_ack_stops_before_next_report},
        TestCase{"verbose_and_silent_are_equivalent", &test_verbose_and_silent_are_equivalent},
        TestCase{"stream_flags_and_fill_are_preserved", &test_stream_flags_and_fill_are_preserved},
    };

    std::size_t failures = 0;
    for (const auto &[name, function] : tests)
    {
        if (!run_test(name, function))
        {
            ++failures;
        }
    }
    if (failures != 0)
    {
        std::cerr << failures << " device test(s) failed\n";
        return 1;
    }
    std::cout << "all device tests passed\n";
    return 0;
}

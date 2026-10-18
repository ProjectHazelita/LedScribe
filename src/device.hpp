#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <libusb.h>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "constants.hpp"

// Descriptor-only description of a compatible display. Discovery never opens,
// claims, configures or transfers to a device; a non-empty discovery_error
// means the record is listed but cannot be selected for writing.
struct DeviceInfo
{
    uint8_t bus{};
    uint8_t address{};
    uint8_t endpoint_out{};
    uint8_t endpoint_in{};
    int interface_number{-1};
    int alternate_setting{};
    int configuration_value{};
    std::vector<uint8_t> port_path;
    std::string discovery_error;

    [[nodiscard]] auto device_id() const -> std::string;
    [[nodiscard]] auto description() const -> std::string;
};

struct WriteProgress
{
    std::size_t reports_completed{};
    std::size_t reports_total{};
};

// Plain C callback boundary: the GUI forwards this to a queued Qt signal without
// letting Qt types leak into the core. notify is never named emit.
struct WriteProgressSink
{
    void *context{nullptr};
    void (*notify)(void *, WriteProgress) noexcept{nullptr};
};

// Enumerates 0416:5020 displays using descriptors only. USB failures that
// prevent describing one display are recorded in its DeviceInfo instead of
// aborting the whole scan.
auto discover_devices(std::ostream *log = nullptr) -> std::vector<DeviceInfo>;

// Resolves exactly one usable display. Throws when there is no usable target,
// more than one target and no explicit selector, the selector is unknown, or
// the selected record carries a discovery_error. Never falls back to the first
// device; errors list the candidate IDs.
auto select_device(std::span<const DeviceInfo> devices,
                   std::optional<std::string_view> selector = std::nullopt) -> DeviceInfo;

class BadgeDevice
{
  public:
    BadgeDevice() = delete;
    ~BadgeDevice();

    BadgeDevice(const BadgeDevice &) = delete;
    auto operator=(const BadgeDevice &) -> BadgeDevice & = delete;
    BadgeDevice(BadgeDevice &&other) noexcept;
    auto operator=(BadgeDevice &&other) noexcept -> BadgeDevice &;

    // Re-enumerates, verifies VID/PID, bus/address and the non-empty USB port
    // path plus interface/endpoints/configuration/alternate, then opens only
    // that target. A vanished or changed device requires a fresh scan.
    static auto open(const DeviceInfo &device, std::ostream *log = nullptr) -> BadgeDevice;

    [[nodiscard]] auto device_id() const -> std::string;
    [[nodiscard]] auto description() const -> std::string;
    auto write(std::span<const uint8_t> payload, std::chrono::milliseconds delay = WRITE_DELAY,
               unsigned int timeout = TRANSFER_TIMEOUT, std::ostream *log = nullptr,
               WriteProgressSink progress = {}) -> void;
    auto close() noexcept -> void;

  private:
    BadgeDevice(std::shared_ptr<libusb_context> context, libusb_device_handle *handle,
                DeviceInfo info);

    std::shared_ptr<libusb_context> context_;
    libusb_device_handle *handle_{nullptr};
    DeviceInfo info_;
};

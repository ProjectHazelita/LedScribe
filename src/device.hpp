#pragma once

#include <chrono>
#include <cstdint>
#include <iosfwd>
#include <libusb.h>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "constants.hpp"

class BadgeDevice
{
  public:
    BadgeDevice() = delete;
    ~BadgeDevice();

    BadgeDevice(const BadgeDevice &) = delete;
    auto operator=(const BadgeDevice &) -> BadgeDevice & = delete;
    BadgeDevice(BadgeDevice &&other) noexcept;
    auto operator=(BadgeDevice &&other) noexcept -> BadgeDevice &;

    static auto find_all(std::ostream *log = nullptr) -> std::vector<BadgeDevice>;

    [[nodiscard]] auto device_id() const -> std::string;
    [[nodiscard]] auto description() const -> std::string;
    auto write(std::span<const uint8_t> payload, std::chrono::milliseconds delay = WRITE_DELAY,
               unsigned int timeout = TRANSFER_TIMEOUT, std::ostream *log = nullptr) -> void;
    auto close() noexcept -> void;

  private:
    BadgeDevice(std::shared_ptr<libusb_context> context, libusb_device_handle *handle,
                uint8_t endpoint_out, uint8_t endpoint_in, int interface_number, uint8_t bus,
                uint8_t address, std::string manufacturer, std::string product);

    std::shared_ptr<libusb_context> context_;
    libusb_device_handle *handle_{nullptr};
    uint8_t endpoint_out_{0};
    uint8_t endpoint_in_{0};
    int interface_number_{-1};
    uint8_t bus_{0};
    uint8_t address_{0};
    std::string manufacturer_;
    std::string product_;
};

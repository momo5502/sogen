#include "../std_include.hpp"
#include "qt_raster.hpp"
#include "../windows_emulator.hpp"
#include "../syscall_utils.hpp"

namespace sogen
{
    NTSTATUS qt_raster_device::io_control(windows_emulator& win_emu, const io_device_context& context)
    {
        if (context.io_status_block)
        {
            context.io_status_block.access([](auto& status) { status.Information = 0; });
        }
        if (context.io_control_code != qt_raster::ioctl)
        {
            return STATUS_NOT_SUPPORTED;
        }
        if (context.input_buffer_length < sizeof(qt_raster::Packet) ||
            context.input_buffer_length > sizeof(qt_raster::Packet) + qt_raster::max_payload)
        {
            return STATUS_INVALID_PARAMETER;
        }
        if (!is_readable_range(win_emu.memory, context.input_buffer, context.input_buffer_length))
        {
            return STATUS_ACCESS_VIOLATION;
        }
        try
        {
            qt_raster::Packet packet{};
            if (!win_emu.emu().try_read_memory(context.input_buffer, &packet, sizeof(packet)))
            {
                return STATUS_ACCESS_VIOLATION;
            }
            if (!qt_raster::valid(packet, context.input_buffer_length))
            {
                return STATUS_INVALID_PARAMETER;
            }
            std::vector<uint8_t> payload(packet.size);
            if (!payload.empty() && !win_emu.emu().try_read_memory(context.input_buffer + sizeof(packet), payload.data(), payload.size()))
            {
                return STATUS_ACCESS_VIOLATION;
            }
            size_t written{};
            const auto result = this->session_.exchange(
                packet, std::move(payload), context.output_buffer_length, [&](const std::span<const uint8_t> bytes) {
                    if (!is_writable_range(win_emu.memory, context.output_buffer, bytes.size()) ||
                        !win_emu.emu().try_write_memory(context.output_buffer, bytes.data(), bytes.size()))
                    {
                        return false;
                    }
                    written = bytes.size();
                    return true;
                });
            switch (result)
            {
            case qt_raster::result::success:
                if (context.io_status_block)
                {
                    context.io_status_block.access([written](auto& status) { status.Information = written; });
                }
                return STATUS_SUCCESS;
            case qt_raster::result::buffer_too_small:
                return STATUS_BUFFER_TOO_SMALL;
            case qt_raster::result::access_violation:
                return STATUS_ACCESS_VIOLATION;
            case qt_raster::result::resource_limit:
                return STATUS_INSUFFICIENT_RESOURCES;
            default:
                return STATUS_INVALID_PARAMETER;
            }
        }
        catch (const std::bad_alloc&)
        {
            return STATUS_INSUFFICIENT_RESOURCES;
        }
    }

    bool qt_raster_device::supports_serialization() const
    {
        return !this->session_.active();
    }

    void qt_raster_device::serialize_object(utils::buffer_serializer&) const
    {
        if (!this->supports_serialization())
        {
            throw std::runtime_error("Active raster sessions cannot be serialized");
        }
    }

    void qt_raster_device::deserialize_object(utils::buffer_deserializer&)
    {
        this->session_.reset();
    }

    qt_raster::session& qt_raster_device::session()
    {
        return this->session_;
    }

    std::unique_ptr<io_device> create_qt_raster(const device_creation_context&)
    {
        return std::make_unique<qt_raster_device>();
    }
}

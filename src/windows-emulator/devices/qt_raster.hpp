#pragma once

#include "../io_device.hpp"
#include <qt_raster_session.hpp>

namespace sogen
{
    class qt_raster_device final : public io_device
    {
      public:
        NTSTATUS io_control(windows_emulator& win_emu, const io_device_context& context) override;
        bool supports_serialization() const override;
        void serialize_object(utils::buffer_serializer& buffer) const override;
        void deserialize_object(utils::buffer_deserializer& buffer) override;

        qt_raster::session& session();

      private:
        qt_raster::session session_{};
    };

    std::unique_ptr<io_device> create_qt_raster(const device_creation_context& context);
}

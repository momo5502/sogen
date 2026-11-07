#pragma once

#include <windows_emulator.hpp>
#include <qt_raster_session.hpp>

namespace sogen::qt_raster
{
    class native_presenter
    {
      public:
        native_presenter(windows_emulator& emulator, ui_backend& ui);
        ~native_presenter();
        native_presenter(const native_presenter&) = delete;
        native_presenter& operator=(const native_presenter&) = delete;

        void update();
        void clear();

      private:
        struct implementation;
        std::unique_ptr<implementation> implementation_;
    };
}

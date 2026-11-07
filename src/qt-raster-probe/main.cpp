#include <windows_emulator.hpp>
#include <backend_selection.hpp>
#include <devices/qt_raster.hpp>

#include <cstring>
#include <fstream>
#include <iostream>
#include <string_view>

namespace
{
    namespace wire = sogen::qt_raster;

    void queue_calculation(wire::session& session, const uint32_t window)
    {
        constexpr std::array<int32_t, 4> keys{'2', '+', '3', 0x01000004};
        for (const auto key : keys)
        {
            for (const int32_t action : {6, 7})
            {
                wire::Packet event{};
                event.op = wire::Key;
                event.id = window;
                event.session = session.identity();
                event.a[0] = action;
                event.a[1] = key;
                const uint8_t text = key == 0x01000004 ? '\r' : static_cast<uint8_t>(key);
                event.size = 1;
                if (!session.enqueue_input(event, {&text, 1}))
                {
                    throw std::runtime_error("Cannot queue calculator input");
                }
            }
        }
    }
}

int main(int argc, char** argv)
{
    if (argc < 4 || argc > 5)
    {
        std::cerr << "Usage: qt-raster-probe <root> <kvm|unicorn> <guest-calculator> [frame-output]\n";
        return 1;
    }
    try
    {
        const std::string_view name{argv[2]};
        if (name != "kvm" && name != "unicorn")
        {
            throw std::invalid_argument("Select kvm or unicorn explicitly");
        }
        const auto backend = name == "kvm" ? sogen::backend_type::kvm : sogen::backend_type::unicorn;
        std::optional<wire::frame> captured{};
        uint64_t frames{};
        bool input_queued{};
        sogen::emulator_callbacks callbacks{};
        callbacks.on_stdout = [](const std::string_view text) { std::cout << text; };
        callbacks.on_ioctrl = [&](sogen::io_device& device, std::u16string_view, uint32_t) {
            auto* raster = dynamic_cast<sogen::qt_raster_device*>(&device);
            if (!raster)
            {
                return;
            }
            auto& session = raster->session();
            if (session.latest_frame())
            {
                captured = session.latest_frame();
                frames = session.frame_count();
                if (!input_queued)
                {
                    queue_calculation(session, captured->window);
                    input_queued = true;
                }
            }
        };
        sogen::application_settings application{.application = argv[3]};
        application.arguments = {u"-platform", u"bridge", u"--probe"};
        sogen::emulator_interfaces interfaces{};
        interfaces.ui = std::make_unique<sogen::null_ui_backend>();
        interfaces.audio = std::make_unique<sogen::null_audio_backend>();
        interfaces.console = std::make_unique<sogen::null_console_backend>();
        sogen::windows_emulator emulator{
            sogen::create_x86_64_emulator(backend), std::move(application),
            sogen::emulator_settings{.disable_logging = true, .emulation_root = argv[1], .root_mode = sogen::filesystem_mode::read_write},
            std::move(callbacks), std::move(interfaces)};
        emulator.start();
        if (emulator.process.exit_status != uint32_t{0} || !input_queued || !captured || frames == 0)
        {
            throw std::runtime_error("Guest painting, input and successful exit were not all observed");
        }
        const auto& bytes = *captured->pixels;
        bool varied{};
        for (size_t i = 4; i < bytes.size(); i += 4)
        {
            varied = varied || std::memcmp(bytes.data(), bytes.data() + i, 4) != 0;
        }
        if (!varied)
        {
            throw std::runtime_error("Guest frame contains only a single pixel value");
        }
        if (argc == 5)
        {
            std::ofstream output(argv[4], std::ios::binary);
            wire::Packet header{};
            header.op = wire::Frame;
            header.id = captured->window;
            header.size = static_cast<uint32_t>(bytes.size());
            header.a[0] = captured->width;
            header.a[1] = captured->height;
            header.a[2] = captured->stride;
            header.a[3] = 1000;
            output.write(reinterpret_cast<const char*>(&header), sizeof(header));
            output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            if (!output)
            {
                throw std::runtime_error("Cannot write the captured frame");
            }
        }
        std::cout << "Guest raster and input passed on " << name << ": " << captured->width << 'x' << captured->height << ", " << frames
                  << " frames\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

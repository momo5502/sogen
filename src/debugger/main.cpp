#include "std_include.hpp"

#include <windows_emulator.hpp>
#include <cstdio>

#ifdef OS_EMSCRIPTEN
#include <emscripten.h>
#endif

#include <utils/finally.hpp>

namespace
{
    void suspend_execution(const std::chrono::milliseconds ms = 0ms)
    {
#ifdef OS_EMSCRIPTEN
        emscripten_sleep(static_cast<uint32_t>(ms.count()));
#else
        if (ms > 0ms)
        {
            std::this_thread::sleep_for(ms);
        }
        else
        {
            std::this_thread::yield();
        }
#endif
    }

    void send_message(const std::string& message)
    {
#ifdef OS_EMSCRIPTEN
        // clang-format off
        EM_ASM_({
                handleMessage(UTF8ToString($0));
        }, message.c_str());
        // clang-format on
#else
        (void)message;
#endif
    }

    std::string receive_message()
    {
#ifdef OS_EMSCRIPTEN
        // clang-format off
        auto* ptr = EM_ASM_PTR({
            var message = getMessageFromQueue();
            if (!message || message.length == 0)
            {
                return null;
            }

            const length = lengthBytesUTF8(message) + 1;
            const buffer = _malloc(length);
            stringToUTF8(message, buffer, length);
            return buffer;
        });
        // clang-format on

        if (!ptr)
        {
            return {};
        }

        const auto _ = utils::finally([&] {
            free(ptr); //
        });

        return {reinterpret_cast<const char*>(ptr)};
#else
        return {};
#endif
    }

    void handle_messages()
    {
        while (true)
        {
            suspend_execution(0ms);
            const auto message = receive_message();
            if (message.empty())
            {
                break;
            }

            puts(message.c_str());
        }
    }

    bool run_emulation(windows_emulator& win_emu)
    {
        try
        {
            win_emu.start();
        }
        catch (const std::exception& e)
        {
            win_emu.log.error("Emulation failed at: 0x%" PRIx64 " - %s\n", win_emu.emu().read_instruction_pointer(),
                              e.what());
            throw;
        }
        catch (...)
        {
            win_emu.log.error("Emulation failed at: 0x%" PRIx64 "\n", win_emu.emu().read_instruction_pointer());
            throw;
        }

        const auto exit_status = win_emu.process.exit_status;
        if (!exit_status.has_value())
        {
            win_emu.log.error("Emulation terminated without status!\n");
            return false;
        }

        const auto success = *exit_status == STATUS_SUCCESS;

        win_emu.log.disable_output(false);
        win_emu.log.print(success ? color::green : color::red, "Emulation terminated with status: %X\n", *exit_status);

        return success;
    }

    bool run(const int argc, char** argv)
    {
        if (argc < 3)
        {
            return false;
        }

        const emulator_settings settings{
            .emulation_root = argv[1],
        };

        application_settings app_settings{
            .application = argv[2],
        };

        for (int i = 3; i < argc; ++i)
        {
            app_settings.arguments.push_back(u8_to_u16(argv[i]));
        }

        windows_emulator win_emu{app_settings, settings};

        win_emu.callbacks.on_thread_switch = [&] {
            handle_messages(); //
        };

        return run_emulation(win_emu);
    }
}

int main(const int argc, char** argv)
{
    try
    {
        const auto result = run(argc, argv);
        return result ? 0 : 1;
    }
    catch (std::exception& e)
    {
        puts(e.what());

#if defined(_WIN32) && 0
        MessageBoxA(nullptr, e.what(), "ERROR", MB_ICONERROR);
#endif
    }

    return 1;
}

#ifdef _WIN32
int WINAPI WinMain(HINSTANCE, HINSTANCE, PSTR, int)
{
    return main(__argc, __argv);
}
#endif

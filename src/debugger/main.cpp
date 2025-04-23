#include "std_include.hpp"

#include <windows_emulator.hpp>
#include <cstdio>

#ifdef OS_EMSCRIPTEN
#include <emscripten.h>
#endif

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

    void sendMessage(const std::string& message)
    {
#ifdef OS_EMSCRIPTEN
        EM_ASM_(
            {
                // JavaScript code to handle the message
                console.log('Received message from C++: ' + UTF8ToString($0));
                // You can post the message to a message queue or handle it as needed
            },
            message.c_str());
#else
        (void)message;
#endif
    }

    // Function to receive a message from JavaScript
    std::string receiveMessage()
    {
        /*#ifdef OS_EMSCRIPTEN
                // Allocate a buffer to receive the message
                char* buffer = (char*)malloc(256); // Adjust size as needed
                EM_ASM_(
                    {
                        // JavaScript code to retrieve the message
                        var message = getMessageFromQueue(); // Implement this function in JavaScript
                        if (message && message.length > 0)
                        {
                            stringToUTF8($0, _malloc(lengthBytesUTF8(message) + 1), lengthBytesUTF8(message) + 1);
                        }
                    },
                    buffer);

                std::string result(buffer);
                free(buffer);
                return result;
        #else*/
        return {};
        // #endif
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

    bool run()
    {
        (void)&run_emulation;

        for (size_t i = 0; i < 10; ++i)
        {
            sendMessage("PING");

            while (true)
            {
                suspend_execution(0ms);
                const auto message = receiveMessage();
                if (message.empty())
                {
                    puts("EMPTY MSG");
                    break;
                }

                puts(message.c_str());
            }

            suspend_execution(1s);
        }

        return true;
    }
}

int main(const int, char**)
{
    try
    {
        const auto result = run();
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

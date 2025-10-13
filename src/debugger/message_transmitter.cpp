#include "message_transmitter.hpp"
#include <platform/compiler.hpp>

#include <thread>

#include <utils/finally.hpp>
#ifdef OS_EMSCRIPTEN
#include <emscripten.h>
#endif

using namespace std::literals;

namespace debugger
{
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
                return 0;
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

    void suspend_execution(const std::chrono::milliseconds ms)
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
}

#include "../std_include.hpp"

#include "macos_appkit_intercept.hpp"

#include "../macos_emulator.hpp"
#include "../macos_platform.hpp"
#include "macos_layer_tree.hpp"

#include <array>
#include <optional>
#include <set>
#include <string>

namespace sogen
{
    namespace
    {
        // The CFConstantString shape macos_layer_tree.cpp reads gravity constants out of: +0x08 info
        // word, +0x10 UTF-8 pointer, +0x18 length.
        constexpr uint64_t CF_CONSTANT_STRING_INFO = 0x7c8;
        constexpr size_t CF_CONSTANT_STRING_MAX_LENGTH = 64;

        constexpr std::string_view APPLICATION_ICON_NAME = "NSApplicationIcon";

        void report_once(macos_emulator& emu, const std::string& key, const std::string& message)
        {
            static std::set<std::string> reported{};
            if (reported.insert(key).second)
            {
                emu.log.warn("%s\n", message.c_str());
            }
        }

        std::optional<std::string> read_constant_string(macos_emulator& emu, const uint64_t address)
        {
            if (address == 0)
            {
                return std::nullopt;
            }

            std::array<uint64_t, 4> fields{};
            if (!emu.memory.try_read_memory(address, fields.data(), sizeof(fields)))
            {
                return std::nullopt;
            }

            if (fields[1] != CF_CONSTANT_STRING_INFO || fields[3] == 0 || fields[3] > CF_CONSTANT_STRING_MAX_LENGTH || fields[2] == 0)
            {
                return std::nullopt;
            }

            std::string text(static_cast<size_t>(fields[3]), '\0');
            if (!emu.memory.try_read_memory(fields[2], text.data(), text.size()))
            {
                return std::nullopt;
            }

            return text;
        }

        // +[NSImage imageNamed:NSImageNameApplicationIcon] is where every application icon on the screen
        // comes from, and its representations are NSISIconImageReps that fetch their pixels from the
        // iconservices agent over NSXPC. sogen models no such agent: the com.apple.iconservices lookup is
        // refused, the fetchCacheConfiguration and generateImage replies come back nil, and the first
        // time a view holding that image updates its layer AppKit reaches NSISIconImageRepGetCGImage,
        // whose assert(result) aborts the process. Measured on 25G76: an NSAlert kills the guest with
        // status 6 out of -[_NSSimpleImageView updateLayer] before any message box can appear.
        //
        // nil is what +[NSImage imageNamed:] already answers for a name it cannot resolve, and it is a
        // state AppKit carries: measured on the host, an alert whose application icon resolves to nil
        // lays out and displays at the same 260x328 and logs "Could not find image named
        // 'NSApplicationIcon'" (src/tools/macos-gui-probe/nilalert.m). Every other name is left alone --
        // only the icon the absent agent would have drawn is refused.
        void image_named(const macos_native_call& call)
        {
            const auto name = read_constant_string(call.emu_ref, call.arg(2));
            if (!name || *name != APPLICATION_ICON_NAME)
            {
                macos_layer_tree_continue_into_original(call);
                return;
            }

            report_once(call.emu_ref, "application-icon",
                        "+[NSImage imageNamed:NSImageNameApplicationIcon] answers nil: sogen models no iconservices "
                        "agent, so the application icon does not exist and anything that would draw it is laid out "
                        "without one");
            call.ret(0);
        }
    }

    std::vector<macos_objc_method> macos_appkit_methods()
    {
        const std::string app_kit{MACOS_APP_KIT_IMAGE_PATH};

        return {
            macos_objc_method{
                .image = app_kit, .class_name = "NSImage", .selector = "imageNamed:", .class_method = true, .handler = image_named},
        };
    }

    std::vector<macos_layer_tree_binding> macos_appkit_bind(macos_emulator& emu, const dyld_shared_cache_reader& cache,
                                                            const macos_cache_symbols& symbols, macos_native_dispatch& dispatch)
    {
        return macos_objc_bind_with_pass_through(emu, cache, symbols, dispatch, macos_appkit_methods());
    }
}

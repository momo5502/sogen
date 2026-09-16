#pragma once

#include "../std_include.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sogen
{
    class macos_emulator;
    class macos_native_dispatch;
    class macos_window_server;

    // Passed to the guest as call arguments, so these are ABI rather than API. kCFStringEncodingUTF8 is
    // CFString.h; the number types are CFNumber.h. kCFAllocatorDefault is NULL by contract, which is why
    // no allocator symbol is resolved.
    constexpr uint64_t MACOS_CF_STRING_ENCODING_UTF8 = 0x08000100;
    constexpr uint64_t MACOS_CF_NUMBER_SINT64 = 4;
    constexpr uint64_t MACOS_CF_NUMBER_FLOAT64 = 6;

    // What a CFArray of space ids says about sogen's one space. Measured 25G76: SLSCopySpacesForWindows
    // returns a space only when the caller's mask is a superset of the space's own property bits, and a
    // window on the login user's active space carries current | user.
    constexpr uint32_t MACOS_CGS_SPACE_INCLUDES_CURRENT = 1u << 0;
    constexpr uint32_t MACOS_CGS_SPACE_INCLUDES_OTHERS = 1u << 1;
    constexpr uint32_t MACOS_CGS_SPACE_INCLUDES_USER = 1u << 2;
    constexpr uint32_t MACOS_CGS_MAIN_SPACE_PROPERTIES = MACOS_CGS_SPACE_INCLUDES_CURRENT | MACOS_CGS_SPACE_INCLUDES_USER;
    constexpr int64_t MACOS_CGS_MAIN_SPACE_ID = 1;

    // CGWindowListOption, CGWindow.h.
    constexpr uint32_t MACOS_CG_WINDOW_LIST_ON_SCREEN_ONLY = 1u << 0;
    constexpr uint32_t MACOS_CG_WINDOW_LIST_ON_SCREEN_ABOVE = 1u << 1;
    constexpr uint32_t MACOS_CG_WINDOW_LIST_ON_SCREEN_BELOW = 1u << 2;
    constexpr uint32_t MACOS_CG_WINDOW_LIST_INCLUDING_WINDOW = 1u << 3;
    constexpr uint32_t MACOS_CG_WINDOW_LIST_EXCLUDE_DESKTOP = 1u << 4;

    // A CoreFoundation object described natively. Nothing here touches the guest: a tree is built, then
    // compiled into a program the guest's own CoreFoundation executes.
    class macos_cf_value
    {
      public:
        enum class kind : uint8_t
        {
            string,
            integer,
            real,
            boolean,
            array,
            dictionary,
        };

        static macos_cf_value string(std::string text);
        static macos_cf_value integer(int64_t value);
        static macos_cf_value real(double value);
        static macos_cf_value boolean(bool value);
        static macos_cf_value array();
        static macos_cf_value dictionary();

        macos_cf_value& append(macos_cf_value value);
        macos_cf_value& set(std::string key, macos_cf_value value);

        kind type() const
        {
            return this->kind_;
        }

        const std::string& text() const
        {
            return this->text_;
        }

        int64_t integer_value() const
        {
            return this->integer_;
        }

        double real_value() const
        {
            return this->real_;
        }

        bool boolean_value() const
        {
            return this->integer_ != 0;
        }

        const std::vector<macos_cf_value>& elements() const
        {
            return this->elements_;
        }

        const std::vector<std::pair<std::string, macos_cf_value>>& entries() const
        {
            return this->entries_;
        }

        const macos_cf_value* find(std::string_view key) const;

      private:
        kind kind_{kind::integer};
        std::string text_{};
        int64_t integer_{};
        double real_{};
        std::vector<macos_cf_value> elements_{};
        std::vector<std::pair<std::string, macos_cf_value>> entries_{};
    };

    struct macos_cf_symbols
    {
        uint64_t array_create_mutable{};
        uint64_t array_append_value{};
        uint64_t dictionary_create_mutable{};
        uint64_t dictionary_set_value{};
        uint64_t string_create_with_bytes{};
        uint64_t number_create{};
        uint64_t release{};

        uint64_t type_array_callbacks{};
        uint64_t type_dictionary_key_callbacks{};
        uint64_t type_dictionary_value_callbacks{};

        // The singletons themselves, already read out of the kCFBooleanTrue/kCFBooleanFalse variables.
        uint64_t boolean_true{};
        uint64_t boolean_false{};

        bool complete() const;
    };

    struct macos_cf_argument
    {
        enum class source : uint8_t
        {
            literal,
            slot,
            scratch,
        };

        source from{source::literal};
        uint64_t value{};
    };

    struct macos_cf_step
    {
        uint64_t function{};
        std::string_view name{};
        std::array<macos_cf_argument, 8> args{};
        size_t argument_count{};

        // -1 discards the result, which is what the void-returning steps do.
        int32_t result_slot{-1};
    };

    // A flat program over a slot file. Slots start at initial_slots -- non-zero only where a CF
    // singleton stands in for an object no call has to create -- and every step writes at most one.
    struct macos_cf_program
    {
        std::vector<uint64_t> initial_slots{};
        std::vector<macos_cf_step> steps{};
        std::vector<uint8_t> scratch{};
        int32_t root_slot{-1};

        bool valid() const
        {
            return this->root_slot >= 0;
        }
    };

    macos_cf_program macos_cf_compile(const macos_cf_symbols& symbols, const macos_cf_value& value);

    // Resolves CoreFoundation's constructors out of the shared cache the guest mapped, and reads the two
    // CFBoolean singletons out of guest memory. Absent before dyld has mapped the cache, and absent on a
    // release that renames any of them -- both of which are reported by name, once.
    std::optional<macos_cf_symbols> macos_cf_resolve(macos_emulator& emu);

    using macos_cf_completion = std::function<void(macos_emulator&, uint64_t root)>;

    // Runs one step per guest call. False when nothing was started, in which case `done` is not called
    // and the caller still owns the return value; otherwise `done` receives the root CFTypeRef, or 0 if
    // the chain could not be finished, and is responsible for writing x0.
    bool macos_cf_run(macos_emulator& emu, macos_cf_program program, macos_cf_completion done);

    bool macos_cf_build(macos_emulator& emu, const macos_cf_value& value, macos_cf_completion done);

    // The answers, as trees. Pure: no emulator, no guest, no allocation in the guest.
    macos_cf_value macos_cf_window_list(const macos_window_server& server, uint32_t option, uint32_t relative_to, uint32_t owner_pid,
                                        std::string_view owner_name);
    macos_cf_value macos_cf_spaces_for_windows(uint32_t mask, size_t window_count);
    macos_cf_value macos_cf_session_properties(uint32_t uid, uint32_t gid);

    void register_cf_container_routines(macos_native_dispatch& dispatch);
}

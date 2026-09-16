#include <gtest/gtest.h>

#include "fixture_utils.hpp"
#include "macos_test_utils.hpp"

#include <gui/macos_cf_bridge.hpp>
#include <gui/macos_guest_call.hpp>
#include <gui/macos_native_dispatch.hpp>
#include <gui/macos_window_server.hpp>
#include <host_range_reader.hpp>
#include <module/dyld_shared_cache.hpp>
#include <module/macos_cache_symbols.hpp>
#include <platform/macho.hpp>

#include <array>
#include <cstring>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

namespace
{
    sogen::macos_cf_symbols fake_symbols()
    {
        return sogen::macos_cf_symbols{
            .array_create_mutable = 0x1001,
            .array_append_value = 0x1002,
            .dictionary_create_mutable = 0x1003,
            .dictionary_set_value = 0x1004,
            .string_create_with_bytes = 0x1005,
            .number_create = 0x1006,
            .release = 0x1007,
            .type_array_callbacks = 0x2001,
            .type_dictionary_key_callbacks = 0x2002,
            .type_dictionary_value_callbacks = 0x2003,
            .boolean_true = 0x3001,
            .boolean_false = 0x3002,
        };
    }

    size_t count_calls(const sogen::macos_cf_program& program, const uint64_t function)
    {
        size_t total = 0;
        for (const auto& step : program.steps)
        {
            total += step.function == function ? 1 : 0;
        }
        return total;
    }

    std::string scratch_string(const sogen::macos_cf_program& program, const size_t offset, const size_t length)
    {
        return std::string{reinterpret_cast<const char*>(program.scratch.data()) + offset, length};
    }

    TEST(CfBridge, CompilesAStringIntoOneCreateAndOneRelease)
    {
        const auto program = sogen::macos_cf_compile(fake_symbols(), sogen::macos_cf_value::string("sogen"));

        ASSERT_TRUE(program.valid());
        ASSERT_EQ(program.steps.size(), 1u) << "the root is never released by the program";

        const auto& step = program.steps.front();
        EXPECT_EQ(step.function, 0x1005u);
        EXPECT_EQ(step.argument_count, 5u);
        EXPECT_EQ(step.args[0].value, 0u) << "kCFAllocatorDefault is NULL";
        EXPECT_EQ(step.args[1].from, sogen::macos_cf_argument::source::scratch);
        EXPECT_EQ(step.args[2].value, 5u);
        EXPECT_EQ(step.args[3].value, sogen::MACOS_CF_STRING_ENCODING_UTF8);
        EXPECT_EQ(step.args[4].value, 0u);
        EXPECT_EQ(scratch_string(program, step.args[1].value, 5), "sogen");
    }

    TEST(CfBridge, NumbersCarryTheMeasuredCFNumberTypes)
    {
        auto root = sogen::macos_cf_value::array();
        root.append(sogen::macos_cf_value::integer(-7));
        root.append(sogen::macos_cf_value::real(0.5));

        const auto program = sogen::macos_cf_compile(fake_symbols(), root);
        ASSERT_TRUE(program.valid());

        std::vector<const sogen::macos_cf_step*> numbers{};
        for (const auto& step : program.steps)
        {
            if (step.function == 0x1006)
            {
                numbers.push_back(&step);
            }
        }

        ASSERT_EQ(numbers.size(), 2u);
        EXPECT_EQ(numbers[0]->args[1].value, sogen::MACOS_CF_NUMBER_SINT64);
        EXPECT_EQ(numbers[1]->args[1].value, sogen::MACOS_CF_NUMBER_FLOAT64);

        int64_t integer = 0;
        std::memcpy(&integer, program.scratch.data() + numbers[0]->args[2].value, sizeof(integer));
        EXPECT_EQ(integer, -7);

        double real = 0.0;
        std::memcpy(&real, program.scratch.data() + numbers[1]->args[2].value, sizeof(real));
        EXPECT_EQ(real, 0.5);
    }

    // Eleven key names repeated once per window would be eleven guest calls per window; the same string
    // is created once and used by every dictionary that names it.
    TEST(CfBridge, IdenticalStringsAreCreatedOnce)
    {
        auto root = sogen::macos_cf_value::array();
        for (int i = 0; i < 3; ++i)
        {
            auto entry = sogen::macos_cf_value::dictionary();
            entry.set("kCGWindowNumber", sogen::macos_cf_value::integer(i));
            root.append(std::move(entry));
        }

        const auto program = sogen::macos_cf_compile(fake_symbols(), root);
        ASSERT_TRUE(program.valid());

        EXPECT_EQ(count_calls(program, 0x1005), 1u) << "one CFStringCreateWithBytes for the repeated key";
        EXPECT_EQ(count_calls(program, 0x1006), 3u) << "the three values are distinct objects";
        EXPECT_EQ(count_calls(program, 0x1003), 3u);
        EXPECT_EQ(count_calls(program, 0x1004), 3u);
    }

    TEST(CfBridge, BooleansAreTheSingletonsAndAreNeverReleased)
    {
        auto root = sogen::macos_cf_value::array();
        root.append(sogen::macos_cf_value::boolean(true));
        root.append(sogen::macos_cf_value::boolean(false));

        const auto symbols = fake_symbols();
        const auto program = sogen::macos_cf_compile(symbols, root);
        ASSERT_TRUE(program.valid());

        EXPECT_EQ(count_calls(program, symbols.release), 0u) << "nothing but the root array was created, and the root is kept";

        std::vector<uint64_t> appended{};
        for (const auto& step : program.steps)
        {
            if (step.function == symbols.array_append_value)
            {
                appended.push_back(program.initial_slots.at(step.args[1].value));
            }
        }

        ASSERT_EQ(appended.size(), 2u);
        EXPECT_EQ(appended[0], symbols.boolean_true);
        EXPECT_EQ(appended[1], symbols.boolean_false);
    }

    // Every reference the program made, except the root, is handed back: the containers retain on
    // insert and the caller of a Copy function owns exactly one reference to the result.
    TEST(CfBridge, EveryCreatedObjectExceptTheRootIsReleasedAfterTheLastInsert)
    {
        auto entry = sogen::macos_cf_value::dictionary();
        entry.set("name", sogen::macos_cf_value::string("window"));
        entry.set("number", sogen::macos_cf_value::integer(3));

        auto root = sogen::macos_cf_value::array();
        root.append(std::move(entry));

        const auto symbols = fake_symbols();
        const auto program = sogen::macos_cf_compile(symbols, root);
        ASSERT_TRUE(program.valid());

        size_t last_insert = 0;
        size_t first_release = program.steps.size();
        for (size_t i = 0; i < program.steps.size(); ++i)
        {
            const auto function = program.steps[i].function;
            if (function == symbols.array_append_value || function == symbols.dictionary_set_value)
            {
                last_insert = i;
            }
            if (function == symbols.release && i < first_release)
            {
                first_release = i;
            }
        }

        EXPECT_LT(last_insert, first_release) << "a release before the last insert would free an object still being stored";

        // Created: the root array, one dictionary, two key strings, one value string, one number. All
        // but the root come back.
        EXPECT_EQ(count_calls(program, symbols.release), 5u);

        for (const auto& step : program.steps)
        {
            if (step.function == symbols.release)
            {
                EXPECT_NE(static_cast<int32_t>(step.args[0].value), program.root_slot);
            }
        }
    }

    TEST(CfBridge, AnIncompleteSymbolSetCompilesToNothing)
    {
        auto symbols = fake_symbols();
        symbols.number_create = 0;

        EXPECT_FALSE(sogen::macos_cf_compile(symbols, sogen::macos_cf_value::integer(1)).valid());
        EXPECT_FALSE(symbols.complete());
    }

    TEST(CfBridge, WindowListCarriesTheMeasuredKeysAndTypes)
    {
        sogen::macos_window_server server{};
        auto* window = server.create_window(sogen::MACOS_MAIN_CONNECTION_ID, 300, 765, 320, 264);
        ASSERT_NE(window, nullptr);
        window->ordered_in = true;
        window->title = u"cfprobe";
        window->backing_stride = 320 * 4;

        const auto list = sogen::macos_cf_window_list(server, sogen::MACOS_CG_WINDOW_LIST_ON_SCREEN_ONLY, 0, 4242, "cfprobe");

        ASSERT_EQ(list.type(), sogen::macos_cf_value::kind::array);
        ASSERT_EQ(list.elements().size(), 1u);

        const auto& entry = list.elements().front();
        ASSERT_EQ(entry.type(), sogen::macos_cf_value::kind::dictionary);
        EXPECT_EQ(entry.entries().size(), 11u) << "the eleven keys measured on 25G76";

        const auto* number = entry.find("kCGWindowNumber");
        ASSERT_NE(number, nullptr);
        EXPECT_EQ(number->type(), sogen::macos_cf_value::kind::integer);
        EXPECT_EQ(number->integer_value(), window->id);

        const auto* onscreen = entry.find("kCGWindowIsOnscreen");
        ASSERT_NE(onscreen, nullptr);
        EXPECT_EQ(onscreen->type(), sogen::macos_cf_value::kind::boolean) << "measured as a CFBoolean, not a CFNumber";
        EXPECT_TRUE(onscreen->boolean_value());

        const auto* alpha = entry.find("kCGWindowAlpha");
        ASSERT_NE(alpha, nullptr);
        EXPECT_EQ(alpha->type(), sogen::macos_cf_value::kind::real) << "measured as kCFNumberFloat64Type";

        const auto* bounds = entry.find("kCGWindowBounds");
        ASSERT_NE(bounds, nullptr);
        ASSERT_EQ(bounds->type(), sogen::macos_cf_value::kind::dictionary);
        EXPECT_EQ(bounds->entries().size(), 4u);
        ASSERT_NE(bounds->find("X"), nullptr);
        EXPECT_EQ(bounds->find("X")->type(), sogen::macos_cf_value::kind::real);
        EXPECT_EQ(bounds->find("X")->real_value(), 300.0);
        EXPECT_EQ(bounds->find("Y")->real_value(), 765.0);
        EXPECT_EQ(bounds->find("Width")->real_value(), 320.0);
        EXPECT_EQ(bounds->find("Height")->real_value(), 264.0);

        ASSERT_NE(entry.find("kCGWindowName"), nullptr);
        EXPECT_EQ(entry.find("kCGWindowName")->text(), "cfprobe");
        ASSERT_NE(entry.find("kCGWindowOwnerPID"), nullptr);
        EXPECT_EQ(entry.find("kCGWindowOwnerPID")->integer_value(), 4242);
    }

    TEST(CfBridge, WindowListHonoursTheOptionBits)
    {
        sogen::macos_window_server server{};

        // create_window hands back a pointer into the server's own vector, and the next create can move
        // it, so only the ids survive the second call.
        const auto below = server.create_window(sogen::MACOS_MAIN_CONNECTION_ID, 0, 0, 100, 100)->id;
        const auto above = server.create_window(sogen::MACOS_MAIN_CONNECTION_ID, 10, 10, 100, 100)->id;
        const auto hidden = server.create_window(sogen::MACOS_MAIN_CONNECTION_ID, 20, 20, 100, 100)->id;

        server.find_window(below)->ordered_in = true;
        server.find_window(above)->ordered_in = true;

        EXPECT_EQ(sogen::macos_cf_window_list(server, 0, 0, 1, "p").elements().size(), 3u) << "kCGWindowListOptionAll";
        EXPECT_EQ(sogen::macos_cf_window_list(server, sogen::MACOS_CG_WINDOW_LIST_ON_SCREEN_ONLY, 0, 1, "p").elements().size(), 2u);
        EXPECT_EQ(sogen::macos_cf_window_list(server, sogen::MACOS_CG_WINDOW_LIST_INCLUDING_WINDOW, hidden, 1, "p").elements().size(), 1u)
            << "including-window reaches a window that is not ordered in";
        EXPECT_EQ(sogen::macos_cf_window_list(server, sogen::MACOS_CG_WINDOW_LIST_ON_SCREEN_ABOVE, below, 1, "p").elements().size(), 1u)
            << "the hidden window above the pivot is still off screen";
        EXPECT_EQ(sogen::macos_cf_window_list(server, sogen::MACOS_CG_WINDOW_LIST_ON_SCREEN_BELOW, above, 1, "p").elements().size(), 1u);
        EXPECT_EQ(sogen::macos_cf_window_list(server, sogen::MACOS_CG_WINDOW_LIST_ON_SCREEN_ABOVE, 0xBEEF, 1, "p").elements().size(), 0u)
            << "above what? a pivot the server does not own selects nothing";
    }

    // Measured on 25G76 with one window on the current space: only a mask that is a superset of the
    // space's own current|user bits returns it.
    TEST(CfBridge, SpacesAreReturnedOnlyForAMaskThatCoversTheSpacesProperties)
    {
        const std::array<size_t, 9> expected{0, 0, 0, 0, 0, 1, 0, 1, 0};

        for (uint32_t mask = 0; mask < expected.size(); ++mask)
        {
            EXPECT_EQ(sogen::macos_cf_spaces_for_windows(mask, 1).elements().size(), expected[mask]) << "mask " << mask;
        }

        EXPECT_EQ(sogen::macos_cf_spaces_for_windows(7, 0).elements().size(), 0u) << "no windows, no spaces";

        const auto spaces = sogen::macos_cf_spaces_for_windows(7, 2);
        ASSERT_EQ(spaces.elements().size(), 1u) << "two windows on one space are one space, measured";
        EXPECT_EQ(spaces.elements().front().type(), sogen::macos_cf_value::kind::integer);
    }

    TEST(CfBridge, SessionPropertiesCarryTheMeasuredKeySet)
    {
        const auto properties = sogen::macos_cf_session_properties(501, 20);

        ASSERT_EQ(properties.type(), sogen::macos_cf_value::kind::dictionary);
        EXPECT_EQ(properties.entries().size(), 12u);

        ASSERT_NE(properties.find("kCGSSessionUserIDKey"), nullptr);
        EXPECT_EQ(properties.find("kCGSSessionUserIDKey")->integer_value(), 501);
        EXPECT_EQ(properties.find("kCGSSessionGroupIDKey")->integer_value(), 20);

        ASSERT_NE(properties.find("kCGSSessionOnConsoleKey"), nullptr);
        EXPECT_EQ(properties.find("kCGSSessionOnConsoleKey")->type(), sogen::macos_cf_value::kind::boolean);
        EXPECT_TRUE(properties.find("kCGSSessionOnConsoleKey")->boolean_value());

        ASSERT_NE(properties.find("kCGSSessionSystemSafeBoot"), nullptr);
        EXPECT_FALSE(properties.find("kCGSSessionSystemSafeBoot")->boolean_value());

        ASSERT_NE(properties.find("CGSSessionUniqueSessionUUID"), nullptr);
        EXPECT_EQ(properties.find("CGSSessionUniqueSessionUUID")->type(), sogen::macos_cf_value::kind::string);
    }

    // The registration the emulator will do, checked against the real cache: a release that renamed one
    // of these three would otherwise show up as a window-server MIG report from unintercepted code.
    TEST(CfBridge, TheContainerRoutinesBindAgainstTheHostCache)
    {
        const std::filesystem::path cache_path{MACOS_DYLD_CACHE_HOST_PATH};
        if (!std::filesystem::exists(cache_path))
        {
            GTEST_SKIP() << "no shared cache on this host";
        }

        const auto emu = macos_test::make_emulator();
        const auto cache = sogen::dyld_shared_cache_reader::open(cache_path);
        const sogen::macos_cache_symbols symbols{cache};

        sogen::macos_native_dispatch dispatch{};
        sogen::register_cf_container_routines(dispatch);
        ASSERT_EQ(dispatch.registered_count(), 3u);

        for (const auto& routine : dispatch.routines())
        {
            const auto address = symbols.find_export(routine.image, routine.symbol);
            ASSERT_TRUE(address.has_value()) << routine.symbol;
            ASSERT_TRUE(emu->memory.allocate_memory(*address & ~(sogen::MACOS_PAGE_SIZE - 1), sogen::MACOS_PAGE_SIZE,
                                                    sogen::memory_permission::read_exec) ||
                        emu->memory.get_region_info(*address).has_value());
        }

        EXPECT_EQ(dispatch.bind(*emu, symbols), 3u);
        EXPECT_TRUE(dispatch.unbound_symbols().empty());
    }

    TEST(CfBridge, NothingIsBuiltWithoutAGuestCallStack)
    {
        const auto emu = macos_test::make_emulator();
        EXPECT_FALSE(sogen::macos_cf_build(*emu, sogen::macos_cf_value::integer(1), [](sogen::macos_emulator&, uint64_t) {}));
    }
}

// The gate: a guest that has loaded the real CoreFoundation, a container this bridge built inside it,
// and the guest's own CFArrayGetCount / CFDictionaryGetValue / CFStringGetCString reading it back.
namespace
{
    constexpr uint64_t PROBE_OFFSET = 0x300;

    bool host_guest_available()
    {
        return std::filesystem::is_regular_file(MACOS_DYLD_HOST_PATH) && std::filesystem::is_regular_file(MACOS_DYLD_CACHE_HOST_PATH);
    }

    // The fixture builder has no LC_LOAD_DYLIB, and CoreFoundation only initialises in a process that
    // links it: the shared cache maps every image but dyld runs initialisers for the dependency graph
    // alone. So this image is built here rather than shared.
    std::vector<uint8_t> build_cf_client(const std::string& dylinker, const std::string& dependency, const std::vector<uint32_t>& code,
                                         const std::vector<uint32_t>& probe)
    {
        constexpr size_t page = 0x4000;
        constexpr uint64_t entry_offset = 0x200;

        const auto dylinker_size = static_cast<uint32_t>((sizeof(sogen::macho::dylinker_command) + dylinker.size() + 1 + 7) & ~7ull);
        const auto dylib_size = static_cast<uint32_t>((sizeof(sogen::macho::dylib_command) + dependency.size() + 1 + 7) & ~7ull);
        const auto entry_size = static_cast<uint32_t>(sizeof(sogen::macho::entry_point_command));

        std::vector<uint8_t> image(page, 0);

        sogen::macho::mach_header_64 header{};
        header.magic = sogen::macho::MH_MAGIC_64;
        header.cputype = sogen::macho::CPU_TYPE_ARM64;
        header.cpusubtype = sogen::macho::CPU_SUBTYPE_ARM64_ALL;
        header.filetype = sogen::macho::MH_EXECUTE;
        header.ncmds = 4;
        header.sizeofcmds = 2 * static_cast<uint32_t>(sizeof(sogen::macho::segment_command_64)) + entry_size + dylinker_size + dylib_size;
        header.flags = sogen::macho::MH_NOUNDEFS;
        header.ncmds = 5;
        std::memcpy(image.data(), &header, sizeof(header));

        size_t cursor = sizeof(header);

        const auto emit_segment = [&](const char* name, const uint64_t vmaddr, const uint64_t vmsize, const uint64_t filesize,
                                      const uint32_t prot) {
            sogen::macho::segment_command_64 segment{};
            segment.cmd = sogen::macho::LC_SEGMENT_64;
            segment.cmdsize = sizeof(segment);
            std::strncpy(segment.segname.data(), name, segment.segname.size());
            segment.vmaddr = vmaddr;
            segment.vmsize = vmsize;
            segment.fileoff = 0;
            segment.filesize = filesize;
            segment.maxprot = prot;
            segment.initprot = prot;
            std::memcpy(image.data() + cursor, &segment, sizeof(segment));
            cursor += sizeof(segment);
        };

        emit_segment("__PAGEZERO", 0, 0x100000000ull, 0, 0);
        emit_segment("__TEXT", 0x100000000ull, page, page, sogen::macho::VM_PROT_READ | sogen::macho::VM_PROT_EXECUTE);

        sogen::macho::entry_point_command entry{};
        entry.cmd = sogen::macho::LC_MAIN;
        entry.cmdsize = entry_size;
        entry.entryoff = entry_offset;
        std::memcpy(image.data() + cursor, &entry, sizeof(entry));
        cursor += entry_size;

        sogen::macho::dylinker_command linker{};
        linker.cmd = sogen::macho::LC_LOAD_DYLINKER;
        linker.cmdsize = dylinker_size;
        linker.name = sizeof(linker);
        std::memcpy(image.data() + cursor, &linker, sizeof(linker));
        std::memcpy(image.data() + cursor + sizeof(linker), dylinker.c_str(), dylinker.size() + 1);
        cursor += dylinker_size;

        sogen::macho::dylib_command dylib{};
        dylib.cmd = sogen::macho::LC_LOAD_DYLIB;
        dylib.cmdsize = dylib_size;
        dylib.name = sizeof(dylib);
        dylib.timestamp = 1;
        dylib.current_version = 0x10000;
        dylib.compatibility_version = 0x10000;
        std::memcpy(image.data() + cursor, &dylib, sizeof(dylib));
        std::memcpy(image.data() + cursor + sizeof(dylib), dependency.c_str(), dependency.size() + 1);

        std::memcpy(image.data() + entry_offset, code.data(), code.size() * sizeof(uint32_t));
        std::memcpy(image.data() + PROBE_OFFSET, probe.data(), probe.size() * sizeof(uint32_t));
        return image;
    }

    struct call_chain
    {
        struct step
        {
            uint64_t function{};
            std::function<std::array<uint64_t, 8>()> arguments{};
            std::function<void(uint64_t)> on_result{};
        };

        std::vector<step> steps{};
        size_t next{};
        bool completed{};
        bool broke{};

        void run(sogen::macos_emulator& emu)
        {
            if (this->next >= this->steps.size())
            {
                this->completed = true;
                emu.emu().reg(sogen::arm64_register::x0, 0);
                return;
            }

            auto& current = this->steps[this->next++];

            sogen::macos_guest_call_request request{.function = current.function};
            if (current.arguments)
            {
                request.args = current.arguments();
            }

            auto* handler = &current;
            request.on_return = [this, handler](sogen::macos_emulator& inner, const uint64_t result) {
                if (handler->on_result)
                {
                    handler->on_result(result);
                }
                this->run(inner);
            };

            if (!emu.guest_call_stack()->begin(emu, std::move(request)))
            {
                this->broke = true;
                this->completed = true;
                emu.emu().reg(sogen::arm64_register::x0, 0);
            }
        }
    };

    struct gate_state
    {
        sogen::macos_emulator* emu{};
        sogen::macos_cf_symbols cf{};
        std::map<std::string, uint64_t> functions{};

        uint64_t scratch{};
        uint64_t root{};
        uint64_t entry_ref{};
        uint64_t key_ref{};
        uint64_t value_ref{};
        uint64_t dictionary_key_ref{};
        uint64_t array_count{};
        uint64_t dictionary_count{};
        uint64_t number_value{};
        std::string text{};
        std::string probe_key_text{};
        std::string dictionary_key_text{};
        bool build_started{};
        bool verified{};

        call_chain chain{};

        std::string read_text(const uint64_t address) const
        {
            std::array<char, 0x40> buffer{};
            this->emu->memory.try_read_memory(address, buffer.data(), buffer.size());
            buffer.back() = '\0';
            return buffer.data();
        }
    };

    gate_state* g_gate = nullptr;

    void gate_probe(const sogen::macos_native_call& call)
    {
        auto& emu = call.emu_ref;

        const auto symbols = sogen::macos_cf_resolve(emu);
        if (!symbols.has_value())
        {
            call.ret(0);
            return;
        }

        g_gate->cf = *symbols;

        // Not a fixed address: the shared cache occupies everything up to MACOS_SHARED_CACHE_END, and it
        // is only mapped once dyld runs, so a page reserved before start() lands inside it.
        g_gate->scratch =
            emu.memory.allocate_memory(sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write, sogen::MACOS_GUI_ARENA_BASE);
        if (g_gate->scratch == 0 || !emu.memory.try_write_memory(g_gate->scratch, "count", 5) ||
            !emu.memory.try_write_memory(g_gate->scratch + 0x10, "name", 4))
        {
            call.ret(0);
            return;
        }

        auto entry = sogen::macos_cf_value::dictionary();
        entry.set("count", sogen::macos_cf_value::integer(7));
        entry.set("name", sogen::macos_cf_value::string("sogen"));

        auto root = sogen::macos_cf_value::array();
        root.append(std::move(entry));

        g_gate->build_started = sogen::macos_cf_build(emu, root, [](sogen::macos_emulator& inner, const uint64_t built) {
            g_gate->root = built;

            auto& gate = *g_gate;
            auto& chain = gate.chain;

            const auto function = [&gate](const char* name) { return gate.functions.at(name); };

            // CFArrayGetCount(root)
            chain.steps.push_back({.function = function("_CFArrayGetCount"),
                                   .arguments = [&gate] { return std::array<uint64_t, 8>{gate.root}; },
                                   .on_result = [&gate](const uint64_t value) { gate.array_count = value; }});

            // entry = CFArrayGetValueAtIndex(root, 0)
            chain.steps.push_back({.function = function("_CFArrayGetValueAtIndex"),
                                   .arguments = [&gate] { return std::array<uint64_t, 8>{gate.root, 0}; },
                                   .on_result = [&gate](const uint64_t value) { gate.entry_ref = value; }});

            chain.steps.push_back({.function = function("_CFDictionaryGetCount"),
                                   .arguments = [&gate] { return std::array<uint64_t, 8>{gate.entry_ref}; },
                                   .on_result = [&gate](const uint64_t value) { gate.dictionary_count = value; }});

            // A key CFString the guest makes for itself: if the guest's own hashing finds the entry
            // under it, the key sogen created is indistinguishable from the exported constant.
            chain.steps.push_back(
                {.function = gate.cf.string_create_with_bytes,
                 .arguments = [&gate] { return std::array<uint64_t, 8>{0, gate.scratch, 5, sogen::MACOS_CF_STRING_ENCODING_UTF8, 0}; },
                 .on_result = [&gate](const uint64_t value) { gate.key_ref = value; }});

            chain.steps.push_back({.function = function("_CFStringGetCString"),
                                   .arguments =
                                       [&gate] {
                                           return std::array<uint64_t, 8>{gate.key_ref, gate.scratch + 0xC0, 0x40,
                                                                          sogen::MACOS_CF_STRING_ENCODING_UTF8};
                                       },
                                   .on_result = [&gate](uint64_t) { gate.probe_key_text = gate.read_text(gate.scratch + 0xC0); }});

            chain.steps.push_back({.function = function("_CFDictionaryGetKeysAndValues"),
                                   .arguments = [&gate] { return std::array<uint64_t, 8>{gate.entry_ref, gate.scratch + 0x100, 0}; },
                                   .on_result =
                                       [&gate](uint64_t) {
                                           uint64_t first = 0;
                                           gate.emu->memory.try_read_memory(gate.scratch + 0x100, &first, sizeof(first));
                                           gate.dictionary_key_ref = first;
                                       }});

            chain.steps.push_back({.function = function("_CFStringGetCString"),
                                   .arguments =
                                       [&gate] {
                                           return std::array<uint64_t, 8>{gate.dictionary_key_ref, gate.scratch + 0x180, 0x40,
                                                                          sogen::MACOS_CF_STRING_ENCODING_UTF8};
                                       },
                                   .on_result = [&gate](uint64_t) { gate.dictionary_key_text = gate.read_text(gate.scratch + 0x180); }});

            chain.steps.push_back({.function = function("_CFDictionaryGetValue"),
                                   .arguments = [&gate] { return std::array<uint64_t, 8>{gate.entry_ref, gate.key_ref}; },
                                   .on_result = [&gate](const uint64_t value) { gate.value_ref = value; }});

            chain.steps.push_back(
                {.function = function("_CFNumberGetValue"),
                 .arguments =
                     [&gate] { return std::array<uint64_t, 8>{gate.value_ref, sogen::MACOS_CF_NUMBER_SINT64, gate.scratch + 0x40}; },
                 .on_result =
                     [&gate](uint64_t) {
                         uint64_t stored = 0;
                         gate.emu->memory.try_read_memory(gate.scratch + 0x40, &stored, sizeof(stored));
                         gate.number_value = stored;
                     }});

            chain.steps.push_back(
                {.function = gate.cf.string_create_with_bytes,
                 .arguments =
                     [&gate] { return std::array<uint64_t, 8>{0, gate.scratch + 0x10, 4, sogen::MACOS_CF_STRING_ENCODING_UTF8, 0}; },
                 .on_result = [&gate](const uint64_t value) { gate.key_ref = value; }});

            chain.steps.push_back({.function = function("_CFDictionaryGetValue"),
                                   .arguments = [&gate] { return std::array<uint64_t, 8>{gate.entry_ref, gate.key_ref}; },
                                   .on_result = [&gate](const uint64_t value) { gate.value_ref = value; }});

            chain.steps.push_back({.function = function("_CFStringGetCString"),
                                   .arguments =
                                       [&gate] {
                                           return std::array<uint64_t, 8>{gate.value_ref, gate.scratch + 0x80, 0x40,
                                                                          sogen::MACOS_CF_STRING_ENCODING_UTF8};
                                       },
                                   .on_result =
                                       [&gate](uint64_t) {
                                           std::array<char, 0x40> buffer{};
                                           gate.emu->memory.try_read_memory(gate.scratch + 0x80, buffer.data(), buffer.size());
                                           buffer.back() = '\0';
                                           gate.text = buffer.data();
                                           gate.verified = true;
                                       }});

            chain.run(inner);
        });

        if (!g_gate->build_started)
        {
            call.ret(0);
        }
    }

    TEST(CfBridge, GateTheGuestsOwnCoreFoundationReadsBackWhatTheBridgeBuilt)
    {
        if (!host_guest_available())
        {
            GTEST_SKIP() << "no host " << MACOS_DYLD_HOST_PATH << " or shared cache";
        }

        const sogen::test::temp_directory scratch{"cf-bridge-gate"};
        std::error_code failure{};
        std::filesystem::create_directories(scratch.path() / "usr" / "lib");
        std::filesystem::create_directories(scratch.path() / "System" / "Library");
        std::filesystem::create_symlink(MACOS_DYLD_HOST_PATH, scratch.path() / "usr" / "lib" / "dyld", failure);
        std::filesystem::create_symlink("/System/Volumes/Preboot/Cryptexes/OS/System/Library/dyld",
                                        scratch.path() / "System" / "Library" / "dyld", failure);

        const std::vector<uint32_t> code{
            0x94000040u, // bl  probe   (0x300 - 0x200) / 4 == 0x40
            0xD2800030u, // mov x16, #1 (exit)
            0xD4001001u, // svc #0x80
        };
        const std::vector<uint32_t> probe{0xD65F03C0u}; // ret, replaced by the native trap

        const auto image = build_cf_client(MACOS_DYLD_HOST_PATH,
                                           "/System/Library/Frameworks/CoreFoundation.framework/Versions/A/CoreFoundation", code, probe);

        std::filesystem::create_directories(scratch.path() / "bin");
        {
            std::ofstream stream{scratch.path() / "bin" / "cfclient", std::ios::binary | std::ios::trunc};
            stream.write(reinterpret_cast<const char*>(image.data()), static_cast<std::streamsize>(image.size()));
        }

        const auto emu = std::make_unique<sogen::macos_emulator>(macos_test::make_backend(), scratch.path());
        ASSERT_TRUE(emu->load_dyld_application("/bin/cfclient", {"/bin/cfclient"}, {}));

        auto cache = sogen::dyld_shared_cache_reader::parse(MACOS_DYLD_CACHE_HOST_PATH,
                                                            sogen::make_host_range_cache_opener(sogen::default_host_range_reader()));
        const sogen::macos_cache_symbols symbols{cache};

        gate_state gate{};
        gate.emu = emu.get();
        for (const auto* name : {"_CFArrayGetCount", "_CFArrayGetValueAtIndex", "_CFDictionaryGetCount", "_CFDictionaryGetValue",
                                 "_CFDictionaryGetKeysAndValues", "_CFNumberGetValue", "_CFStringGetCString"})
        {
            const auto address = symbols.find_export("/System/Library/Frameworks/CoreFoundation.framework/Versions/A/CoreFoundation", name);
            ASSERT_TRUE(address.has_value()) << name;
            gate.functions[name] = *address;
        }
        g_gate = &gate;

        sogen::macos_guest_call_stack calls{};
        ASSERT_TRUE(calls.prepare(*emu));
        emu->set_guest_call_stack(&calls);

        const auto* executable = emu->mod_manager.executable;
        ASSERT_NE(executable, nullptr);
        const auto probe_address = executable->image_base + PROBE_OFFSET;

        sogen::macos_native_dispatch dispatch{};
        dispatch.bind_entry(probe_address, "CfBridgeGateProbe", gate_probe);
        ASSERT_TRUE(sogen::patch_native_entry(*emu, probe_address));
        emu->set_native_dispatch(&dispatch);

        emu->start(2'000'000'000);
        g_gate = nullptr;

        ASSERT_TRUE(gate.build_started) << "the bridge never reached the guest's CoreFoundation; stop reason "
                                        << static_cast<int>(emu->last_stop_reason()) << " " << emu->last_stop_detail();
        ASSERT_NE(gate.root, 0u) << "CFArrayCreateMutable returned NULL inside the guest";
        ASSERT_TRUE(gate.verified) << "the read-back chain did not finish; stop reason " << static_cast<int>(emu->last_stop_reason()) << " "
                                   << emu->last_stop_detail() << "\n  array_count=" << gate.array_count
                                   << " dictionary_count=" << gate.dictionary_count << " entry=" << std::hex << gate.entry_ref
                                   << " key=" << gate.key_ref << " value=" << gate.value_ref << std::dec << " probe_key='"
                                   << gate.probe_key_text << "' dictionary_key='" << gate.dictionary_key_text << "'";

        EXPECT_EQ(gate.array_count, 1u) << "the guest's own CFArrayGetCount";
        EXPECT_EQ(gate.dictionary_count, 2u) << "the guest's own CFDictionaryGetCount";
        EXPECT_EQ(gate.number_value, 7u) << "read back through CFDictionaryGetValue with a key the guest hashed itself";
        EXPECT_EQ(gate.text, "sogen");
        EXPECT_EQ(gate.probe_key_text, "count") << "the key the verification chain hashed";
        EXPECT_TRUE(gate.dictionary_key_text == "count" || gate.dictionary_key_text == "name")
            << "the guest's own CFDictionaryGetKeysAndValues sees the keys the bridge created, not the bytes it was handed: "
            << gate.dictionary_key_text;
        EXPECT_EQ(emu->last_stop_reason(), sogen::stop_reason::normal_exit);

        // The scratch the bridge handed CFStringCreateWithBytes came out of the GUI arena, so it is
        // still mapped now that the build has finished. Releasing it would put the address back in the
        // pool the guest's own allocator draws from while the guest is still running CoreFoundation.
        EXPECT_EQ(emu->ui.arena.block_count(), 1u);
        EXPECT_EQ(emu->ui.arena.retired_count(), 0u);
    }
}

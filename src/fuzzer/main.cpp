#include "std_include.hpp"

#include <windows_emulator.hpp>
#include <fuzzer.hpp>

#include <utils/finally.hpp>

#if MOMO_ENABLE_RUST_CODE
#include <icicle_x86_64_emulator.hpp>
#endif

#ifdef _MSC_VER
#pragma warning(disable : 4702)
#endif

bool use_gdb = false;

namespace
{
    std::unique_ptr<x86_64_emulator> create_emulator_backend()
    {
#if MOMO_ENABLE_RUST_CODE
        return icicle::create_x86_64_emulator();
#else
        throw std::runtime_error("Fuzzer requires rust code to be enabled");
#endif
    }

    void run_emulation(windows_emulator& win_emu)
    {
        bool has_exception = false;
        const auto _ = utils::finally([&] {
            win_emu.callbacks.on_exception = {}; //
        });

        try
        {
            win_emu.callbacks.on_exception = [&] {
                has_exception = true;
                win_emu.stop();
            };

            win_emu.log.disable_output(true);
            win_emu.start();

            if (has_exception)
            {
                throw std::runtime_error("Exception!");
            }
        }
        catch (...)
        {
            win_emu.log.disable_output(false);
            win_emu.log.error("Emulation failed at: 0x%" PRIx64 "\n", win_emu.emu().read_instruction_pointer());
            throw;
        }

        win_emu.log.disable_output(false);
    }

    void forward_emulator(windows_emulator& win_emu)
    {
        const auto target = win_emu.mod_manager.executable->find_export("vulnerable");
        win_emu.emu().hook_memory_execution(target, [&](uint64_t) {
            win_emu.emu().stop(); //
        });

        run_emulation(win_emu);
    }

    struct fuzzer_executer : fuzzer::executer
    {
        windows_emulator emu{create_emulator_backend()};
        std::span<const std::byte> emulator_data{};
        std::unordered_set<uint64_t> visited_blocks{};
        const std::function<fuzzer::coverage_functor>* handler{nullptr};

        fuzzer_executer(const std::span<const std::byte> data)
            : emulator_data(data)
        {
            emu.emu().hook_basic_block([&](const basic_block& block) {
                if (this->handler && visited_blocks.emplace(block.address).second)
                {
                    (*this->handler)(block.address);
                }
            });

            utils::buffer_deserializer deserializer{emulator_data};
            emu.deserialize(deserializer);
            emu.save_snapshot();

            const auto return_address = emu.emu().read_stack(0);
            emu.emu().hook_memory_execution(return_address, [&](const uint64_t) {
                emu.emu().stop(); //
            });
        }

        void restore_emulator()
        {
            /*utils::buffer_deserializer deserializer{ emulator_data };
            emu.deserialize(deserializer);*/
            emu.restore_snapshot();
        }

        fuzzer::execution_result execute(const std::span<const uint8_t> data,
                                         const std::function<fuzzer::coverage_functor>& coverage_handler) override
        {
            // printf("Input size: %zd\n", data.size());
            this->handler = &coverage_handler;
            this->visited_blocks.clear();

            restore_emulator();

            const auto memory = emu.memory.allocate_memory(
                static_cast<size_t>(page_align_up(std::max(data.size(), static_cast<size_t>(1)))), memory_permission::read_write);
            emu.emu().write_memory(memory, data.data(), data.size());

            emu.emu().reg(x86_register::rcx, memory);
            emu.emu().reg<uint64_t>(x86_register::rdx, data.size());

            try
            {
                run_emulation(emu);
                return fuzzer::execution_result::success;
            }
            catch (...)
            {
                return fuzzer::execution_result::error;
            }
        }
    };

    struct my_fuzzing_handler : fuzzer::fuzzing_handler
    {
        std::vector<std::byte> emulator_state{};
        std::atomic_bool stop_fuzzing{false};

        my_fuzzing_handler(std::vector<std::byte> emulator_state)
            : emulator_state(std::move(emulator_state))
        {
        }

        std::unique_ptr<fuzzer::executer> make_executer() override
        {
            return std::make_unique<fuzzer_executer>(emulator_state);
        }

        bool stop() override
        {
            return stop_fuzzing;
        }
    };

    void run_fuzzer(const windows_emulator& base_emulator)
    {
        const auto concurrency = std::thread::hardware_concurrency() + 4;

        utils::buffer_serializer serializer{};
        base_emulator.serialize(serializer);

        my_fuzzing_handler handler{serializer.move_buffer()};

        fuzzer::run(handler, concurrency);
    }

    void run(const std::string_view application)
    {
        application_settings settings{
            .application = application,
        };

        windows_emulator win_emu{create_emulator_backend(), std::move(settings)};

        forward_emulator(win_emu);
        run_fuzzer(win_emu);
    }

    int run_main(const int argc, char** argv)
    {
        if (argc <= 1)
        {
            puts("Application not specified!");
            return 1;
        }

        // setvbuf(stdout, nullptr, _IOFBF, 0x10000);
        if (argc > 2 && argv[1] == "-d"s)
        {
            use_gdb = true;
        }

        try
        {
            do
            {
                run(argv[use_gdb ? 2 : 1]);
            } while (use_gdb);

            return 0;
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
}

int main(const int argc, char** argv)
{
    return run_main(argc, argv);
}

#ifdef _WIN32
int WINAPI WinMain(HINSTANCE, HINSTANCE, PSTR, int)
{
    return run_main(__argc, __argv);
}
#endif

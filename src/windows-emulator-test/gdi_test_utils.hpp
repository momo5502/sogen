#pragma once

#include "emulation_test_utils.hpp"

#include <syscall_utils.hpp>

#include <atomic>

namespace sogen::syscalls
{
    uint64_t handle_NtGdiCreateCompatibleDC(const syscall_context& c, hdc dc);
}

namespace sogen::test
{
    class GdiFallbackTest : public testing::Test
    {
      protected:
        void SetUp() override
        {
            static std::atomic_uint64_t counter{};
            const auto path = std::filesystem::temp_directory_path() /
                              ("sogen-gdi-fallback-" + std::to_string(getpid()) + "-" + std::to_string(counter++));
            ASSERT_TRUE(std::filesystem::create_directory(path));
            this->directory = path;
            ASSERT_TRUE(std::filesystem::create_directories(this->directory / "filesys" / "c" / "windows" / "Fonts"));

            emulator_interfaces interfaces{};
            interfaces.ui = std::make_unique<null_ui_backend>();
            interfaces.audio = std::make_unique<null_audio_backend>();
            interfaces.console = std::make_unique<null_console_backend>();
            this->emu = std::make_unique<windows_emulator>(
                create_x86_64_emulator_from_environment(),
                emulator_settings{.disable_logging = true, .emulation_root = this->directory, .load_registry = false}, emulator_callbacks{},
                std::move(interfaces));
            this->emu->version.set_system_root(windows_path{"C:\\Windows"});

            constexpr size_t allocation_size = 0x400000;
            const auto base =
                this->emu->memory.allocate_memory(allocation_size, memory_permission::read_write, false, DEFAULT_ALLOCATION_ADDRESS_32BIT,
                                                  memory_region_kind::private_allocation, 0xFFFFFFFF);
            ASSERT_NE(base, 0);
            auto& process = this->emu->process;
            process.base_allocator = emulator_allocator{this->emu->memory, base, allocation_size};
            process.peb64 = process.base_allocator.reserve<PEB64>();
            process.peb64.write({});
            process.user_handles.setup(false);

            const auto thread_handle = process.threads.store(emulator_thread{this->emu->memory});
            auto* thread = process.threads.get(thread_handle);
            ASSERT_NE(thread, nullptr);
            thread->teb64 = process.base_allocator.reserve<TEB64>();
            thread->teb64->write({});
            this->emu->vcpu(0).active_thread = thread;

            this->dc = static_cast<hdc>(syscalls::handle_NtGdiCreateCompatibleDC(this->context(), 0));
            ASSERT_NE(this->dc, 0);
        }

        void TearDown() override
        {
            this->emu.reset();
            if (!this->directory.empty())
            {
                std::error_code error{};
                std::filesystem::remove_all(this->directory, error);
            }
        }

        syscall_context context()
        {
            return {.win_emu = *this->emu, .emu = this->emu->vcpu(0).cpu, .vcpu = this->emu->vcpu(0), .proc = this->emu->process};
        }

        std::unique_ptr<windows_emulator> emu{};
        hdc dc{};

      private:
        std::filesystem::path directory{};
    };
}

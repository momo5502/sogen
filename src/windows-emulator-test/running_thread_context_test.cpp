#include "emulation_test_utils.hpp"
#include <syscall_utils.hpp>
#include <utils/finally.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <thread>
#include <utility>
#include <vector>

namespace sogen::syscalls
{
    NTSTATUS handle_NtGetContextThread(const syscall_context& c, handle thread_handle, emulator_object<CONTEXT64> thread_context);
    NTSTATUS handle_NtSetContextThread(const syscall_context& c, handle thread_handle, emulator_object<CONTEXT64> thread_context);
    NTSTATUS handle_NtSetInformationThread(const syscall_context& c, handle thread_handle, THREADINFOCLASS info_class,
                                           uint64_t thread_information, uint32_t thread_information_length);
    NTSTATUS handle_NtTerminateThread(const syscall_context& c, handle thread_handle, NTSTATUS exit_status);
    NTSTATUS handle_NtClose(const syscall_context& c, handle h);
}

namespace sogen::test
{
    namespace
    {
        constexpr uint64_t stale_rax = 0xDEAD;
        constexpr uint64_t first_rax = 0x1001;
        constexpr uint64_t second_rax = 0x2001;
        constexpr uint64_t written_rax = 0x3001;
        constexpr uint64_t untouched_rax = 0x4001;
        constexpr uint64_t thread_ip = 0x10000;
        constexpr uint32_t reserved_eax = 0x5001;
        constexpr uint32_t written_eax = 0x6001;
        constexpr uint32_t written_dr7 = 0x401;
        constexpr uint32_t get_context_syscall_id = 0x42;
        constexpr uint64_t instrumentation_callback = 0x20000;

        template <typename Cpu>
        class register_file_cpu : public Cpu
        {
          public:
            std::atomic<size_t> stops{0};

            void start(size_t /*count*/) override
            {
                throw std::runtime_error("Test vCPUs do not execute code");
            }

            void stop() override
            {
                ++this->stops;
            }

            size_t read_raw_register(const int reg, void* value, const size_t size) override
            {
                std::memset(value, 0, size);
                std::memcpy(value, this->slot(reg).data(), std::min(size, slot_size));
                return size;
            }

            size_t write_raw_register(const int reg, const void* value, const size_t size) override
            {
                auto slot = this->slot(reg);
                std::ranges::fill(slot, std::byte{});
                std::memcpy(slot.data(), value, std::min(size, slot_size));
                return size;
            }

            std::vector<std::byte> save_registers() const override
            {
                return this->registers_;
            }

            void restore_registers(const std::vector<std::byte>& register_data) override
            {
                if (register_data.size() != this->registers_.size())
                {
                    throw std::invalid_argument("Register data of another CPU type");
                }

                this->registers_ = register_data;
            }

            bool has_violation() const override
            {
                return false;
            }

            bool supports_instruction_counting() const override
            {
                return false;
            }

            bool is_stop_thread_safe() const override
            {
                return true;
            }

            bool read_descriptor_table(int /*reg*/, x86_64_cpu::descriptor_table_register& /*table*/) override
            {
                return false;
            }

            void set_segment_base(const x86_register base, const uint64_t value) override
            {
                this->reg(segment_base_register(base), value);
            }

            uint64_t get_segment_base(const x86_register base) override
            {
                return this->reg(segment_base_register(base));
            }

            void load_gdt(uint64_t /*address*/, uint32_t /*limit*/) override
            {
            }

          private:
            static constexpr size_t slot_size = 64;
            std::vector<std::byte> registers_ = std::vector<std::byte>(static_cast<size_t>(x86_register::end) * slot_size);

            static x86_register segment_base_register(const x86_register base)
            {
                return base == x86_register::fs ? x86_register::fs_base : x86_register::gs_base;
            }

            std::span<std::byte> slot(const int reg)
            {
                return std::span(this->registers_).subspan(static_cast<size_t>(reg) * slot_size, slot_size);
            }
        };

        class second_vcpu final : public register_file_cpu<x86_64_cpu>
        {
          public:
            explicit second_vcpu(memory_interface& memory)
                : memory_(&memory)
            {
            }

            size_t index() const override
            {
                return 1;
            }

            memory_interface& memory() override
            {
                return *this->memory_;
            }

            const memory_interface& memory() const override
            {
                return *this->memory_;
            }

          private:
            memory_interface* memory_{};
        };

        // Guest memory and registers without code execution, with a second vCPU, as no backend outside Windows (WHP)
        // runs more than one.
        class two_vcpu_machine final : public register_file_cpu<x86_64_emulator>
        {
          public:
            second_vcpu& second()
            {
                return this->second_;
            }

            std::string get_name() const override
            {
                return "two-vCPU test machine";
            }

            bool supports_multiple_vcpus() const override
            {
                return true;
            }

            size_t vcpu_count() const override
            {
                return 2;
            }

            x86_64_cpu& get_cpu(const size_t index) override
            {
                if (index == 1)
                {
                    return this->second_;
                }

                return register_file_cpu::get_cpu(index);
            }

            void serialize_state(utils::buffer_serializer& buffer, bool /*is_snapshot*/) const override
            {
                buffer.write_vector(this->save_registers());
            }

            void deserialize_state(utils::buffer_deserializer& buffer, bool /*is_snapshot*/) override
            {
                this->restore_registers(buffer.read_vector<std::byte>());
            }

            void read_memory(const uint64_t address, void* data, const size_t size) const override
            {
                if (!this->try_read_memory(address, data, size))
                {
                    throw std::runtime_error("Read from unmapped memory");
                }
            }

            bool try_read_memory(const uint64_t address, void* data, const size_t size) const override
            {
                const auto* source = locate(this->regions_, address, size);
                if (source)
                {
                    std::memcpy(data, source, size);
                }

                return source;
            }

            void write_memory(const uint64_t address, const void* data, const size_t size) override
            {
                if (!this->try_write_memory(address, data, size))
                {
                    throw std::runtime_error("Write to unmapped memory");
                }
            }

            bool try_write_memory(const uint64_t address, const void* data, const size_t size) override
            {
                auto* target = locate(this->regions_, address, size);
                if (target)
                {
                    std::memcpy(target, data, size);
                }

                return target;
            }

            emulator_hook* hook_memory_execution(memory_execution_hook_callback /*callback*/) override
            {
                return nullptr;
            }

            emulator_hook* hook_memory_execution(uint64_t /*address*/, memory_execution_hook_callback /*callback*/) override
            {
                return nullptr;
            }

            emulator_hook* hook_memory_range_execution(uint64_t /*address*/, uint64_t /*size*/,
                                                       memory_execution_hook_callback /*callback*/) override
            {
                return nullptr;
            }

            emulator_hook* hook_memory_read(uint64_t /*address*/, uint64_t /*size*/, memory_access_hook_callback /*callback*/) override
            {
                return nullptr;
            }

            emulator_hook* hook_memory_write(uint64_t /*address*/, uint64_t /*size*/, memory_access_hook_callback /*callback*/) override
            {
                return nullptr;
            }

            emulator_hook* hook_interrupt(interrupt_hook_callback /*callback*/) override
            {
                return nullptr;
            }

            emulator_hook* hook_memory_violation(memory_violation_hook_callback /*callback*/) override
            {
                return nullptr;
            }

            emulator_hook* hook_basic_block(basic_block_hook_callback /*callback*/) override
            {
                return nullptr;
            }

            void delete_hook(emulator_hook* /*hook*/) override
            {
            }

          private:
            second_vcpu second_{*this};
            std::map<uint64_t, std::vector<std::byte>> regions_{};

            template <typename Regions>
            static auto locate(Regions& regions, const uint64_t address, const size_t size) -> decltype(regions.begin()->second.data())
            {
                auto region = regions.upper_bound(address);
                if (region == regions.begin())
                {
                    return nullptr;
                }

                --region;
                const auto offset = address - region->first;
                if (offset + size > region->second.size())
                {
                    return nullptr;
                }

                return region->second.data() + offset;
            }

            emulator_hook* hook_instruction(int /*instruction_type*/, instruction_hook_callback /*callback*/) override
            {
                return nullptr;
            }

            void map_mmio(const uint64_t address, const size_t size, mmio_read_callback /*read_cb*/,
                          mmio_write_callback /*write_cb*/) override
            {
                this->map_memory(address, size, memory_permission::read_write);
            }

            void map_memory(const uint64_t address, const size_t size, memory_permission /*permissions*/) override
            {
                this->regions_.insert_or_assign(address, std::vector<std::byte>(size));
            }

            void unmap_memory(const uint64_t address, size_t /*size*/) override
            {
                this->regions_.erase(address);
            }

            void apply_memory_protection(uint64_t /*address*/, size_t /*size*/, memory_permission /*permissions*/) override
            {
            }
        };

        // Stops the emulator if the test has not finished in time, which ends every wait a test can hang in.
        class watchdog
        {
          public:
            explicit watchdog(windows_emulator& emu)
                : thread_([this, &emu] {
                      std::unique_lock lock{this->mutex_};
                      if (!this->finished_cv_.wait_for(lock, std::chrono::seconds(10), [this] { return this->finished_; }))
                      {
                          this->expired_ = true;
                          emu.stop();
                      }
                  })
            {
            }

            ~watchdog()
            {
                {
                    const std::scoped_lock lock{this->mutex_};
                    this->finished_ = true;
                }

                this->finished_cv_.notify_one();
                this->thread_.join();
                EXPECT_FALSE(this->expired_) << "The test did not finish within 10 seconds";
            }

            watchdog(const watchdog&) = delete;
            watchdog& operator=(const watchdog&) = delete;
            watchdog(watchdog&&) = delete;
            watchdog& operator=(watchdog&&) = delete;

          private:
            std::mutex mutex_{};
            std::condition_variable finished_cv_{};
            bool finished_{false};
            bool expired_{false};
            std::thread thread_;
        };

        class RunningThreadContextTest : public testing::Test
        {
          protected:
            void SetUp() override
            {
                auto cpu = std::make_unique<two_vcpu_machine>();
                this->machine = cpu.get();
                emulator_interfaces interfaces{};
                interfaces.audio = std::make_unique<null_audio_backend>();
                interfaces.console = std::make_unique<null_console_backend>();
                interfaces.ui = std::make_unique<null_ui_backend>();
                this->emu = std::make_unique<windows_emulator>(
                    std::move(cpu),
                    emulator_settings{.disable_logging = true, .emulation_root = get_emulator_root(), .load_registry = false},
                    emulator_callbacks{}, std::move(interfaces));
                this->emu->process.default_register_set = this->machine->save_registers();
                this->first = this->run_thread(this->emu->vcpu(0), 8, first_rax);
                this->second = this->run_thread(this->emu->vcpu(1), 12, second_rax);
                this->storage = this->emu->memory.allocate_memory(0x1000, memory_permission::read_write);
                this->watchdog_.emplace(*this->emu);
            }

            void TearDown() override
            {
                this->watchdog_.reset();
            }

            handle create_thread(const uint32_t id)
            {
                return this->emu->process.threads.store(
                    emulator_thread{this->emu->memory, this->emu->process, 0, 0, 0x100000, 0, id, true});
            }

            handle run_thread(vcpu_context& vcpu, const uint32_t id, const uint64_t live_rax)
            {
                const auto thread_handle = this->create_thread(id);
                auto* thread = this->emu->process.threads.get(thread_handle);
                thread->setup_if_necessary(vcpu.cpu, this->emu->process);
                vcpu.x64_cpu().reg(x86_register::rip, thread_ip);
                vcpu.x64_cpu().reg(x86_register::rax, stale_rax);
                thread->save(vcpu.cpu);
                EXPECT_TRUE(this->emu->activate_thread(vcpu, id));
                vcpu.x64_cpu().reg(x86_register::rax, live_rax);
                return thread_handle;
            }

            syscall_context context(const size_t vcpu)
            {
                return {.win_emu = *this->emu, .emu = this->emu->vcpu(vcpu).cpu, .vcpu = this->emu->vcpu(vcpu), .proc = this->emu->process};
            }

            emulator_object<CONTEXT64> context_buffer(const uint64_t offset, const uint32_t flags, const uint64_t rax = 0)
            {
                const emulator_object<CONTEXT64> buffer{this->emu->memory, this->storage + offset};
                CONTEXT64 value{};
                value.ContextFlags = flags;
                value.Rax = rax;
                buffer.write(value);
                return buffer;
            }

            uint64_t rax(const size_t vcpu)
            {
                return this->emu->vcpu(vcpu).x64_cpu().reg(x86_register::rax);
            }

            uint64_t rip(const size_t vcpu)
            {
                return this->emu->vcpu(vcpu).x64_cpu().reg(x86_register::rip);
            }

            // What the worker of vCPU 1 does when its guest code stops: switch threads, if it was asked to.
            std::future<bool> run_second_vcpu_worker(const std::atomic_bool& caller_returned)
            {
                return std::async(std::launch::async, [this, &caller_returned] {
                    auto& vcpu = this->emu->vcpu(1);
                    while (!caller_returned)
                    {
                        if (vcpu.switch_thread)
                        {
                            return this->emu->perform_thread_switch(vcpu);
                        }

                        std::this_thread::yield();
                    }

                    return false;
                });
            }

            emulator_thread& thread(const handle thread_handle)
            {
                return *this->emu->process.threads.get(thread_handle);
            }

            bool thread_exists(const handle thread_handle)
            {
                return this->emu->synchronize([&] {
                    return this->emu->process.threads.get(thread_handle) != nullptr; //
                });
            }

            bool wait_for_deletion(const handle thread_handle)
            {
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
                while (this->thread_exists(thread_handle))
                {
                    if (std::chrono::steady_clock::now() > deadline)
                    {
                        return false;
                    }

                    std::this_thread::yield();
                }

                return true;
            }

            template <typename Result>
            void wait_for_kick(const vcpu_context& vcpu, const std::future<Result>& caller)
            {
                while (!vcpu.switch_thread && caller.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
                {
                    std::this_thread::yield();
                }
            }

            // Issues a syscall from vCPU 0 through the dispatcher's forwarding, which writes the status and rewinds a
            // retriggered syscall.
            template <typename... Args>
            std::future<void> syscall_from_first_vcpu(NTSTATUS (*handler)(const syscall_context&, Args...),
                                                      const std::array<uint64_t, sizeof...(Args)> arguments)
            {
                constexpr std::array argument_registers{x86_register::r10, x86_register::rdx, x86_register::r8, x86_register::r9};
                for (size_t i = 0; i < arguments.size(); ++i)
                {
                    this->emu->vcpu(0).x64_cpu().reg(argument_registers.at(i), arguments.at(i));
                }

                return std::async(std::launch::async, [this, handler] {
                    this->emu->dispatch_on_cpu(this->emu->vcpu(0).cpu, [&] {
                        const auto c = this->context(0);
                        forward_syscall(c, handler);
                    });
                });
            }

            void register_get_context_syscall()
            {
                std::array<std::byte, 8> stub{std::byte{0x4C}, std::byte{0x8B}, std::byte{0xD1}, std::byte{0xB8}};
                std::memcpy(stub.data() + 4, &get_context_syscall_id, sizeof(get_context_syscall_id));
                this->emu->dispatcher.setup({exported_symbol{.name = "NtGetContextThread"}}, stub, {}, {});
            }

            // Issues NtGetContextThread from vCPU 0 through the dispatcher, as the syscall hook does.
            std::future<void> dispatch_get_context_from_first_vcpu(const handle thread_handle, const emulator_object<CONTEXT64> buffer)
            {
                auto& cpu = this->emu->vcpu(0).x64_cpu();
                cpu.reg(x86_register::eax, get_context_syscall_id);
                cpu.reg(x86_register::r10, thread_handle.bits);
                cpu.reg(x86_register::rdx, buffer.value());
                return std::async(std::launch::async, [this] {
                    this->emu->dispatch_on_cpu(this->emu->vcpu(0).cpu, [this] {
                        this->emu->dispatcher.dispatch(*this->emu, this->emu->vcpu(0)); //
                    });
                });
            }

            uint64_t retriggered_ip() const
            {
                return thread_ip - this->emu->abi().syscall_abi().syscall_instruction_size();
            }

            uint64_t after_syscall_ip() const
            {
                return thread_ip + this->emu->abi().syscall_abi().syscall_instruction_size();
            }

            uint64_t saved_register(const handle thread_handle, const x86_register reg)
            {
                auto& cpu = this->emu->vcpu(0).x64_cpu();
                const auto live = cpu.save_registers();
                this->thread(thread_handle).load_registers(cpu);
                const auto value = cpu.reg(reg);
                cpu.restore_registers(live);
                return value;
            }

            emulator_object<WOW64_CPURESERVED> give_wow64_context(const handle thread_handle)
            {
                this->emu->process.is_wow64_process = true;
                const auto address = this->emu->memory.allocate_memory(0x1000, memory_permission::read_write);
                const emulator_object<WOW64_CPURESERVED> reserved{this->emu->memory, address};
                WOW64_CPURESERVED value{};
                value.Context.Eax = reserved_eax;
                reserved.write(value);
                this->thread(thread_handle).wow64_cpu_reserved = reserved;
                return reserved;
            }

            emulator_object<WOW64_CONTEXT> wow64_debug_register_write()
            {
                const emulator_object<WOW64_CONTEXT> buffer{this->emu->memory, this->storage + 0x800};
                WOW64_CONTEXT value{};
                value.ContextFlags = CONTEXT_DEBUG_REGISTERS_32 | CONTEXT_INTEGER_32;
                value.Dr7 = written_dr7;
                value.Eax = written_eax;
                buffer.write(value);
                return buffer;
            }

            // A syscall on vCPU 1 in a host call that runs with the kernel lock released until released is set.
            std::future<void> unlocked_host_wait_on_second_vcpu(std::atomic_bool& entered, const std::atomic_bool& released)
            {
                return std::async(std::launch::async, [this, &entered, &released] {
                    this->emu->dispatch_on_cpu(this->emu->vcpu(1).cpu, [&] {
                        this->emu->run_without_kernel_lock([&] {
                            entered = true;
                            this->wait_until(released);
                        });
                    });
                });
            }

            // The same host call made by a syscall handler on vCPU 1, which writes the status and rewinds a moved IP.
            std::future<void> unlocked_syscall_on_second_vcpu(std::atomic_bool& entered, const std::atomic_bool& released)
            {
                return std::async(std::launch::async, [this, &entered, &released] {
                    this->emu->dispatch_on_cpu(this->emu->vcpu(1).cpu, [&] {
                        const auto c = this->context(1);
                        const auto ip = c.emu.read_instruction_pointer();
                        this->emu->run_without_kernel_lock([&] {
                            entered = true;
                            this->wait_until(released);
                        });
                        write_syscall_result(c, STATUS_SUCCESS, ip);
                    });
                });
            }

            static void wait_until(const std::atomic_bool& flag)
            {
                while (!flag)
                {
                    std::this_thread::yield();
                }
            }

            template <typename Handler>
            std::future<NTSTATUS> access_from_first_vcpu(Handler handler, const handle thread_handle,
                                                         const emulator_object<CONTEXT64> buffer)
            {
                return std::async(std::launch::async, [this, handler, thread_handle, buffer] {
                    return this->emu->dispatch_on_cpu(this->emu->vcpu(0).cpu, [&] {
                        return handler(this->context(0), thread_handle, buffer); //
                    });
                });
            }

            std::unique_ptr<windows_emulator> emu{};
            std::optional<watchdog> watchdog_{};
            two_vcpu_machine* machine{};
            handle first{};
            handle second{};
            uint64_t storage{};
        };
    }

    TEST_F(RunningThreadContextTest, GetReturnsTheLiveRegistersOfAThreadRunningOnAnotherVcpu)
    {
        const auto buffer = this->context_buffer(0, CONTEXT64_ALL);
        std::atomic_bool caller_returned{false};
        auto second_vcpu = this->run_second_vcpu_worker(caller_returned);

        const auto status = this->emu->dispatch_on_cpu(this->emu->vcpu(0).cpu, [&] {
            return syscalls::handle_NtGetContextThread(this->context(0), this->second, buffer); //
        });
        caller_returned = true;

        EXPECT_EQ(STATUS_SUCCESS, status);
        EXPECT_EQ(second_rax, buffer.read().Rax);
        EXPECT_GT(this->machine->second().stops.load(), 0u);
        EXPECT_TRUE(second_vcpu.get());
        EXPECT_EQ(&this->thread(this->second), this->emu->vcpu(1).active_thread);
        EXPECT_EQ(second_rax, this->rax(1));
        EXPECT_EQ(first_rax, this->rax(0));
        EXPECT_EQ(0u, this->thread(this->second).pending_register_accesses);
    }

    TEST_F(RunningThreadContextTest, SetSurvivesTheSwitchOfAThreadRunningOnAnotherVcpu)
    {
        const auto buffer = this->context_buffer(0, CONTEXT_INTEGER_64, written_rax);
        std::atomic_bool caller_returned{false};
        auto second_vcpu = this->run_second_vcpu_worker(caller_returned);

        const auto status = this->emu->dispatch_on_cpu(this->emu->vcpu(0).cpu, [&] {
            return syscalls::handle_NtSetContextThread(this->context(0), this->second, buffer); //
        });
        caller_returned = true;

        EXPECT_EQ(STATUS_SUCCESS, status);
        EXPECT_TRUE(second_vcpu.get());
        EXPECT_EQ(&this->thread(this->second), this->emu->vcpu(1).active_thread);
        EXPECT_EQ(written_rax, this->rax(1));
        EXPECT_EQ(first_rax, this->rax(0));
    }

    TEST_F(RunningThreadContextTest, ThreadsOnTwoVcpusCanReadEachOthersContext)
    {
        const auto first_buffer = this->context_buffer(0, CONTEXT64_ALL);
        const auto second_buffer = this->context_buffer(0x800, CONTEXT64_ALL);
        std::atomic_bool first_returned{false};

        auto second_vcpu = std::async(std::launch::async, [&] {
            auto& vcpu = this->emu->vcpu(1);
            while (!vcpu.switch_thread && !first_returned)
            {
                std::this_thread::yield();
            }

            const auto status = this->emu->dispatch_on_cpu(vcpu.cpu, [&] {
                return syscalls::handle_NtGetContextThread(this->context(1), this->first, second_buffer); //
            });
            return status == STATUS_SUCCESS && this->emu->perform_thread_switch(vcpu);
        });

        auto first_vcpu = std::async(std::launch::async, [&] {
            const auto status = this->emu->dispatch_on_cpu(this->emu->vcpu(0).cpu, [&] {
                return syscalls::handle_NtGetContextThread(this->context(0), this->second, first_buffer); //
            });
            first_returned = true;
            return status;
        });

        EXPECT_EQ(STATUS_SUCCESS, first_vcpu.get());
        EXPECT_TRUE(second_vcpu.get());
        EXPECT_EQ(second_rax, first_buffer.read().Rax);
        EXPECT_EQ(first_rax, second_buffer.read().Rax);
        EXPECT_EQ(&this->thread(this->second), this->emu->vcpu(1).active_thread);
        EXPECT_EQ(second_rax, this->rax(1));
        EXPECT_EQ(first_rax, this->rax(0));
    }

    TEST_F(RunningThreadContextTest, ThreadExitingDuringAnAccessIsDeletedOnlyAfterIt)
    {
        const auto buffer = this->context_buffer(0, CONTEXT64_ALL);
        const auto exists_during_access = std::make_shared<std::optional<bool>>();
        const auto idle_thread = this->create_thread(9);
        this->thread(idle_thread).await_host_condition = [exists_during_access, &threads = this->emu->process.threads,
                                                          target = this->second] {
            if (!*exists_during_access)
            {
                *exists_during_access = threads.get(target) != nullptr;
            }

            return false;
        };

        auto caller = this->access_from_first_vcpu(syscalls::handle_NtGetContextThread, this->second, buffer);
        auto& vcpu = this->emu->vcpu(1);
        this->wait_for_kick(vcpu, caller);
        this->emu->dispatch_on_cpu(vcpu.cpu, [&] {
            syscalls::handle_NtClose(this->context(1), this->second);
            syscalls::handle_NtTerminateThread(this->context(1), this->second, STATUS_SUCCESS);
        });
        auto second_vcpu = std::async(std::launch::async, [&] { return this->emu->perform_thread_switch(vcpu); });
        const auto _ = utils::finally([&] { this->emu->stop(); });

        EXPECT_EQ(STATUS_SUCCESS, caller.get());
        EXPECT_EQ(second_rax, this->emu->synchronize([&] { return buffer.read().Rax; }));
        EXPECT_EQ(std::optional{true}, *exists_during_access);
        EXPECT_TRUE(this->wait_for_deletion(this->second));
    }

    TEST_F(RunningThreadContextTest, ThreadClosedAfterExitingDuringAnAccessIsDeletedOnlyAfterIt)
    {
        const auto buffer = this->context_buffer(0, CONTEXT64_ALL);
        auto caller = this->access_from_first_vcpu(syscalls::handle_NtGetContextThread, this->second, buffer);
        auto& vcpu = this->emu->vcpu(1);
        this->wait_for_kick(vcpu, caller);

        const auto exists_during_access = this->emu->dispatch_on_cpu(vcpu.cpu, [&] {
            syscalls::handle_NtTerminateThread(this->context(1), this->second, STATUS_SUCCESS);
            syscalls::handle_NtClose(this->context(1), this->second);
            return this->emu->process.threads.get(this->second) != nullptr;
        });
        auto second_vcpu = std::async(std::launch::async, [&] { return this->emu->perform_thread_switch(vcpu); });
        const auto _ = utils::finally([&] { this->emu->stop(); });

        EXPECT_TRUE(exists_during_access);
        EXPECT_EQ(STATUS_SUCCESS, caller.get());
        EXPECT_EQ(second_rax, this->emu->synchronize([&] { return buffer.read().Rax; }));
        EXPECT_TRUE(this->wait_for_deletion(this->second));
    }

    TEST_F(RunningThreadContextTest, GetInterruptedByAStopIsRetriedWithoutReturningRegisters)
    {
        const auto buffer = this->context_buffer(0, CONTEXT64_ALL, untouched_rax);
        auto caller = this->syscall_from_first_vcpu(syscalls::handle_NtGetContextThread, {this->second.bits, buffer.value()});
        this->wait_for_kick(this->emu->vcpu(1), caller);
        this->emu->stop();
        caller.get();

        EXPECT_EQ(first_rax, this->rax(0));
        EXPECT_EQ(this->retriggered_ip(), this->emu->vcpu(0).x64_cpu().reg(x86_register::rip));
        EXPECT_EQ(untouched_rax, buffer.read().Rax);
        EXPECT_EQ(&this->thread(this->second), this->emu->vcpu(1).active_thread);
        EXPECT_EQ(second_rax, this->rax(1));
        EXPECT_EQ(0u, this->thread(this->second).pending_register_accesses);
        EXPECT_FALSE(this->emu->vcpu(0).active_thread_saved);
    }

    TEST_F(RunningThreadContextTest, SetInterruptedByAStopIsRetriedWithoutWritingRegisters)
    {
        const auto buffer = this->context_buffer(0, CONTEXT_INTEGER_64, written_rax);
        auto caller = this->syscall_from_first_vcpu(syscalls::handle_NtSetContextThread, {this->second.bits, buffer.value()});
        this->wait_for_kick(this->emu->vcpu(1), caller);
        this->emu->stop();
        caller.get();

        EXPECT_EQ(first_rax, this->rax(0));
        EXPECT_EQ(this->retriggered_ip(), this->emu->vcpu(0).x64_cpu().reg(x86_register::rip));
        EXPECT_EQ(second_rax, this->rax(1));
        EXPECT_EQ(stale_rax, this->saved_register(this->second, x86_register::rax));
        EXPECT_EQ(0u, this->thread(this->second).pending_register_accesses);
        EXPECT_FALSE(this->emu->vcpu(0).active_thread_saved);
    }

    TEST_F(RunningThreadContextTest, WowDebugRegistersReachAThreadRunningOnAnotherVcpu)
    {
        const auto reserved = this->give_wow64_context(this->second);
        const auto buffer = this->wow64_debug_register_write();
        std::atomic_bool caller_returned{false};
        auto second_vcpu = this->run_second_vcpu_worker(caller_returned);

        const auto status = this->emu->dispatch_on_cpu(this->emu->vcpu(0).cpu, [&] {
            return syscalls::handle_NtSetInformationThread(this->context(0), this->second, ThreadWow64Context, buffer.value(),
                                                           sizeof(WOW64_CONTEXT));
        });
        caller_returned = true;

        EXPECT_EQ(STATUS_SUCCESS, status);
        EXPECT_TRUE(second_vcpu.get());
        EXPECT_EQ(written_eax, reserved.read().Context.Eax);
        EXPECT_EQ(&this->thread(this->second), this->emu->vcpu(1).active_thread);
        EXPECT_EQ(written_dr7, this->emu->vcpu(1).x64_cpu().reg(x86_register::dr7));
        EXPECT_EQ(second_rax, this->rax(1));
        EXPECT_EQ(first_rax, this->rax(0));
    }

    TEST_F(RunningThreadContextTest, WowDebugRegisterWriteInterruptedByAStopIsRetriedWithoutWritingAnything)
    {
        const auto reserved = this->give_wow64_context(this->second);
        const auto buffer = this->wow64_debug_register_write();
        auto caller = this->syscall_from_first_vcpu(syscalls::handle_NtSetInformationThread,
                                                    {this->second.bits, ThreadWow64Context, buffer.value(), sizeof(WOW64_CONTEXT)});
        this->wait_for_kick(this->emu->vcpu(1), caller);
        this->emu->stop();
        caller.get();

        EXPECT_EQ(first_rax, this->rax(0));
        EXPECT_EQ(this->retriggered_ip(), this->emu->vcpu(0).x64_cpu().reg(x86_register::rip));
        EXPECT_EQ(0, reserved.read().Flags);
        EXPECT_EQ(reserved_eax, reserved.read().Context.Eax);
        EXPECT_EQ(0u, this->saved_register(this->second, x86_register::dr7));
        EXPECT_EQ(0u, this->thread(this->second).pending_register_accesses);
    }

    TEST_F(RunningThreadContextTest, ThreadRunningOnAnotherVcpuIsDeletedOnlyAfterItsVcpuUnloadsIt)
    {
        const auto exists_after_close = this->emu->dispatch_on_cpu(this->emu->vcpu(0).cpu, [&] {
            syscalls::handle_NtTerminateThread(this->context(0), this->second, STATUS_SUCCESS);
            syscalls::handle_NtClose(this->context(0), this->second);
            return this->emu->process.threads.get(this->second) != nullptr;
        });
        ASSERT_TRUE(exists_after_close);

        auto& vcpu = this->emu->vcpu(1);
        auto second_vcpu = std::async(std::launch::async, [&] { return this->emu->perform_thread_switch(vcpu); });
        const auto _ = utils::finally([&] { this->emu->stop(); });

        EXPECT_TRUE(this->wait_for_deletion(this->second));
        EXPECT_EQ(nullptr, this->emu->synchronize([&] { return vcpu.active_thread; }));
    }

    TEST_F(RunningThreadContextTest, CallerTerminatedAndClosedDuringItsAccessIsDeletedOnlyAfterIt)
    {
        const auto buffer = this->context_buffer(0, CONTEXT64_ALL);
        auto caller = this->access_from_first_vcpu(syscalls::handle_NtGetContextThread, this->second, buffer);
        auto& vcpu = this->emu->vcpu(1);
        this->wait_for_kick(vcpu, caller);

        const auto exists_after_close = this->emu->dispatch_on_cpu(vcpu.cpu, [&] {
            syscalls::handle_NtTerminateThread(this->context(1), this->first, STATUS_SUCCESS);
            syscalls::handle_NtClose(this->context(1), this->first);
            return this->emu->process.threads.get(this->first) != nullptr;
        });
        EXPECT_TRUE(exists_after_close);

        std::future<bool> first_vcpu{};
        auto second_vcpu = std::async(std::launch::async, [&] { return this->emu->perform_thread_switch(vcpu); });
        const auto _ = utils::finally([&] { this->emu->stop(); });

        EXPECT_EQ(STATUS_SUCCESS, caller.get());
        EXPECT_EQ(second_rax, this->emu->synchronize([&] { return buffer.read().Rax; }));
        EXPECT_TRUE(this->thread_exists(this->first));

        first_vcpu = std::async(std::launch::async, [&] { return this->emu->perform_thread_switch(this->emu->vcpu(0)); });
        EXPECT_TRUE(this->wait_for_deletion(this->first));
    }

    TEST_F(RunningThreadContextTest, ThreadInTransitIsDeletedOnlyAfterTheTransfer)
    {
        const auto transferred = this->create_thread(16);
        const auto exists_after_close = this->emu->dispatch_on_cpu(this->emu->vcpu(0).cpu, [&] {
            this->thread(transferred).registers_in_transit = true;
            syscalls::handle_NtTerminateThread(this->context(0), transferred, STATUS_SUCCESS);
            syscalls::handle_NtClose(this->context(0), transferred);
            return this->emu->process.threads.get(transferred) != nullptr;
        });
        ASSERT_TRUE(exists_after_close);

        this->emu->synchronize([&] { this->thread(transferred).registers_in_transit = false; });
        auto second_vcpu = std::async(std::launch::async, [&] { return this->emu->perform_thread_switch(this->emu->vcpu(1)); });
        const auto _ = utils::finally([&] { this->emu->stop(); });

        EXPECT_TRUE(this->wait_for_deletion(transferred));
    }

    TEST_F(RunningThreadContextTest, GetDoesNotWaitForAThreadInAnUnlockedHostWait)
    {
        const auto buffer = this->context_buffer(0, CONTEXT64_ALL);
        std::atomic_bool entered{false};
        std::atomic_bool released{false};
        auto host_wait = this->unlocked_host_wait_on_second_vcpu(entered, released);
        const auto release = utils::finally([&] { released = true; });
        this->wait_until(entered);

        const auto status = this->access_from_first_vcpu(syscalls::handle_NtGetContextThread, this->second, buffer).get();
        released = true;
        host_wait.get();

        EXPECT_EQ(STATUS_SUCCESS, status);
        EXPECT_EQ(second_rax, buffer.read().Rax);
        EXPECT_EQ(0u, this->machine->second().stops.load());
        EXPECT_EQ(&this->thread(this->second), this->emu->vcpu(1).active_thread);
        EXPECT_EQ(second_rax, this->rax(1));
        EXPECT_EQ(first_rax, this->rax(0));
        EXPECT_FALSE(this->emu->vcpu(1).active_thread_saved);
    }

    TEST_F(RunningThreadContextTest, SetDuringAnUnlockedHostWaitIsReloadedWhenTheWaitEnds)
    {
        const auto buffer = this->context_buffer(0, CONTEXT_INTEGER_64, written_rax);
        std::atomic_bool entered{false};
        std::atomic_bool released{false};
        auto host_wait = this->unlocked_host_wait_on_second_vcpu(entered, released);
        const auto release = utils::finally([&] { released = true; });
        this->wait_until(entered);

        const auto status = this->access_from_first_vcpu(syscalls::handle_NtSetContextThread, this->second, buffer).get();
        released = true;
        host_wait.get();

        EXPECT_EQ(STATUS_SUCCESS, status);
        EXPECT_EQ(&this->thread(this->second), this->emu->vcpu(1).active_thread);
        EXPECT_EQ(written_rax, this->rax(1));
        EXPECT_EQ(first_rax, this->rax(0));
    }

    TEST_F(RunningThreadContextTest, ThreadInTransitIsNotKicked)
    {
        const auto buffer = this->context_buffer(0, CONTEXT64_ALL);
        this->emu->synchronize([&] { this->thread(this->second).registers_in_transit = true; });
        auto caller = this->access_from_first_vcpu(syscalls::handle_NtGetContextThread, this->second, buffer);
        while (this->emu->synchronize([&] { return this->thread(this->second).pending_register_accesses == 0; }))
        {
            std::this_thread::yield();
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        const auto stops_in_transit = this->machine->second().stops.load();
        this->emu->synchronize([&] { this->thread(this->second).registers_in_transit = false; });

        std::atomic_bool caller_returned{false};
        auto second_vcpu = this->run_second_vcpu_worker(caller_returned);
        const auto status = caller.get();
        caller_returned = true;

        EXPECT_EQ(0u, stops_in_transit);
        EXPECT_EQ(STATUS_SUCCESS, status);
        EXPECT_EQ(second_rax, buffer.read().Rax);
        EXPECT_TRUE(second_vcpu.get());
    }

    TEST_F(RunningThreadContextTest, GetWaitsForAThreadSwitchedOutForAnotherOne)
    {
        const auto buffer = this->context_buffer(0, CONTEXT64_ALL);
        const auto ready = this->create_thread(20);
        this->thread(ready).setup_if_necessary(this->emu->vcpu(1).cpu, this->emu->process);
        std::atomic_bool caller_returned{false};
        auto second_vcpu = this->run_second_vcpu_worker(caller_returned);

        const auto status = this->emu->dispatch_on_cpu(this->emu->vcpu(0).cpu, [&] {
            return syscalls::handle_NtGetContextThread(this->context(0), this->second, buffer); //
        });
        caller_returned = true;

        EXPECT_EQ(STATUS_SUCCESS, status);
        EXPECT_EQ(second_rax, buffer.read().Rax);
        EXPECT_TRUE(second_vcpu.get());
        EXPECT_EQ(&this->thread(ready), this->emu->vcpu(1).active_thread);
        EXPECT_EQ(first_rax, this->rax(0));
    }

    TEST_F(RunningThreadContextTest, WaitForAVcpuThatIgnoresTheKickBlocksInsteadOfSpinning)
    {
        const auto buffer = this->context_buffer(0, CONTEXT64_ALL);
        auto caller = this->access_from_first_vcpu(syscalls::handle_NtGetContextThread, this->second, buffer);
        this->wait_for_kick(this->emu->vcpu(1), caller);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        const auto kicks = this->machine->second().stops.load();

        std::atomic_bool caller_returned{false};
        auto second_vcpu = this->run_second_vcpu_worker(caller_returned);
        const auto status = caller.get();
        caller_returned = true;

        EXPECT_LT(kicks, 500u);
        EXPECT_EQ(STATUS_SUCCESS, status);
        EXPECT_TRUE(second_vcpu.get());
    }

    TEST_F(RunningThreadContextTest, ThreadInAnUnlockedHostCallShowsTheInstructionAfterItsSyscall)
    {
        const auto buffer = this->context_buffer(0, CONTEXT_CONTROL_64 | CONTEXT_INTEGER_64);
        std::atomic_bool entered{false};
        std::atomic_bool released{false};
        auto host_call = this->unlocked_syscall_on_second_vcpu(entered, released);
        const auto release = utils::finally([&] { released = true; });
        this->wait_until(entered);

        const auto get_status = this->access_from_first_vcpu(syscalls::handle_NtGetContextThread, this->second, buffer).get();
        const auto set_status = this->access_from_first_vcpu(syscalls::handle_NtSetContextThread, this->second, buffer).get();
        released = true;
        host_call.get();

        EXPECT_EQ(STATUS_SUCCESS, get_status);
        EXPECT_EQ(STATUS_SUCCESS, set_status);
        EXPECT_EQ(this->after_syscall_ip(), buffer.read().Rip);
        EXPECT_EQ(thread_ip, this->rip(1));
        EXPECT_EQ(STATUS_SUCCESS, this->rax(1));
        EXPECT_EQ(first_rax, this->rax(0));
    }

    TEST_F(RunningThreadContextTest, WaitingCallerShowsTheInstructionAfterItsSyscallAndIsStillRetried)
    {
        const auto buffer = this->context_buffer(0, CONTEXT64_ALL, untouched_rax);
        auto caller = this->syscall_from_first_vcpu(syscalls::handle_NtGetContextThread, {this->second.bits, buffer.value()});
        this->wait_for_kick(this->emu->vcpu(1), caller);

        const auto caller_context = this->context_buffer(0x800, CONTEXT_CONTROL_64 | CONTEXT_INTEGER_64);
        const auto [get_status, set_status] = this->emu->dispatch_on_cpu(this->emu->vcpu(1).cpu, [&] {
            return std::pair{syscalls::handle_NtGetContextThread(this->context(1), this->first, caller_context),
                             syscalls::handle_NtSetContextThread(this->context(1), this->first, caller_context)};
        });
        this->emu->stop();
        caller.get();

        EXPECT_EQ(STATUS_SUCCESS, get_status);
        EXPECT_EQ(STATUS_SUCCESS, set_status);
        EXPECT_EQ(this->after_syscall_ip(), caller_context.read().Rip);
        EXPECT_EQ(this->retriggered_ip(), this->rip(0));
        EXPECT_EQ(first_rax, this->rax(0));
        EXPECT_EQ(untouched_rax, buffer.read().Rax);
    }

    TEST_F(RunningThreadContextTest, RetriedSyscallIsReportedAndRedirectedOnlyOnce)
    {
        this->register_get_context_syscall();
        this->emu->process.instrumentation_callback = instrumentation_callback;
        size_t reported{0};
        this->emu->callbacks.on_syscall = [&](uint32_t, std::string_view) {
            ++reported;
            return instruction_hook_continuation::run_instruction;
        };

        const auto buffer = this->context_buffer(0, CONTEXT64_ALL);
        auto interrupted = this->dispatch_get_context_from_first_vcpu(this->second, buffer);
        this->wait_for_kick(this->emu->vcpu(1), interrupted);
        this->emu->stop();
        interrupted.get();
        const auto interrupted_ip = this->rip(0);
        const auto interrupted_r10 = this->emu->vcpu(0).x64_cpu().reg(x86_register::r10);
        const auto reported_when_interrupted = reported;

        std::atomic_bool entered{false};
        std::atomic_bool released{false};
        auto host_call = this->unlocked_host_wait_on_second_vcpu(entered, released);
        const auto release = utils::finally([&] { released = true; });
        this->wait_until(entered);
        this->emu->vcpu(0).x64_cpu().reg(x86_register::rip, interrupted_ip + this->emu->abi().syscall_abi().syscall_instruction_size());
        this->dispatch_get_context_from_first_vcpu(this->second, buffer).get();
        released = true;
        host_call.get();

        EXPECT_EQ(this->retriggered_ip(), interrupted_ip);
        EXPECT_EQ(this->second.bits, interrupted_r10);
        EXPECT_EQ(1u, reported_when_interrupted);
        EXPECT_EQ(1u, reported);
        EXPECT_EQ(second_rax, buffer.read().Rax);
        EXPECT_EQ(STATUS_SUCCESS, this->rax(0));
        EXPECT_EQ(thread_ip, this->emu->vcpu(0).x64_cpu().reg(x86_register::r10));
        EXPECT_EQ(instrumentation_callback - this->emu->abi().syscall_abi().syscall_instruction_size(), this->rip(0));
    }

    TEST_F(RunningThreadContextTest, DeserializedThreadIsDeletedOnlyAfterItsVcpuUnloadsIt)
    {
        utils::buffer_serializer state{};
        this->emu->synchronize([&] {
            this->emu->vcpu(1).active_thread = nullptr;
            this->emu->serialize(state);
        });
        utils::buffer_deserializer deserializer{state};
        this->emu->synchronize([&] { this->emu->deserialize(deserializer); });

        auto& vcpu = this->emu->vcpu(0);
        ASSERT_EQ(&this->thread(this->first), vcpu.active_thread);
        ASSERT_EQ(&vcpu, vcpu.active_thread->last_vcpu);
        ASSERT_TRUE(this->emu->activate_thread(this->emu->vcpu(1), 12));

        const auto exists_after_close = this->emu->dispatch_on_cpu(this->emu->vcpu(1).cpu, [&] {
            syscalls::handle_NtTerminateThread(this->context(1), this->first, STATUS_SUCCESS);
            syscalls::handle_NtClose(this->context(1), this->first);
            return this->emu->process.threads.get(this->first) != nullptr;
        });
        EXPECT_TRUE(exists_after_close);

        auto first_vcpu = std::async(std::launch::async, [&] { return this->emu->perform_thread_switch(vcpu); });
        const auto _ = utils::finally([&] { this->emu->stop(); });
        EXPECT_TRUE(this->wait_for_deletion(this->first));
    }

    TEST_F(RunningThreadContextTest, FailedReloadAfterAnUnlockedHostCallReachesTheCaller)
    {
        std::atomic_bool entered{false};
        std::atomic_bool released{false};
        auto host_call = std::async(std::launch::async, [&] {
            return this->emu->dispatch_on_cpu(this->emu->vcpu(1).cpu, [&] {
                try
                {
                    this->emu->run_without_kernel_lock([&] {
                        entered = true;
                        this->wait_until(released);
                    });
                    return false;
                }
                catch (const std::exception&)
                {
                    return &this->emu->active_cpu() == &this->emu->vcpu(1).cpu;
                }
            });
        });
        const auto release = utils::finally([&] { released = true; });
        this->wait_until(entered);
        this->emu->synchronize([&] { this->thread(this->second).last_registers.clear(); });
        released = true;

        EXPECT_TRUE(host_call.get());
        EXPECT_FALSE(this->emu->vcpu(1).active_thread_saved);
        EXPECT_EQ(second_rax, this->rax(1));
    }

    TEST_F(RunningThreadContextTest, HostCallThatThrowsLeavesItsThreadLoaded)
    {
        const std::atomic_bool fails{true};
        const auto thrown = this->emu->dispatch_on_cpu(this->emu->vcpu(1).cpu, [&] {
            try
            {
                this->emu->run_without_kernel_lock([&] {
                    if (fails)
                    {
                        throw std::runtime_error("Host call failed");
                    }
                });
                return false;
            }
            catch (const std::runtime_error&)
            {
                return &this->emu->active_cpu() == &this->emu->vcpu(1).cpu;
            }
        });

        EXPECT_TRUE(thrown);
        EXPECT_FALSE(this->emu->vcpu(1).active_thread_saved);
        EXPECT_EQ(&this->thread(this->second), this->emu->vcpu(1).active_thread);
        EXPECT_EQ(second_rax, this->rax(1));
    }

    TEST_F(RunningThreadContextTest, KickThatIsLostIsRepeated)
    {
        const auto buffer = this->context_buffer(0, CONTEXT64_ALL);
        auto caller = this->access_from_first_vcpu(syscalls::handle_NtGetContextThread, this->second, buffer);
        auto& vcpu = this->emu->vcpu(1);
        const auto kicks_under_lock = [this] { return this->emu->synchronize([this] { return this->machine->second().stops.load(); }); };
        this->wait_for_kick(vcpu, caller);
        const auto first_kicks = kicks_under_lock();

        this->emu->synchronize([&] { vcpu.switch_thread = false; });
        this->wait_for_kick(vcpu, caller);
        const auto kicked_again = vcpu.switch_thread.load();
        const auto kicks = kicks_under_lock();

        std::atomic_bool caller_returned{false};
        auto second_vcpu = this->run_second_vcpu_worker(caller_returned);
        const auto status = caller.get();
        caller_returned = true;

        EXPECT_TRUE(kicked_again);
        EXPECT_GT(kicks, first_kicks);
        EXPECT_EQ(STATUS_SUCCESS, status);
        EXPECT_EQ(second_rax, buffer.read().Rax);
        EXPECT_TRUE(second_vcpu.get());
    }

    TEST_F(RunningThreadContextTest, SetOfAWaitingCallerIsInPlaceWhenItContinues)
    {
        const auto buffer = this->context_buffer(0, CONTEXT64_ALL);
        auto caller = this->access_from_first_vcpu(syscalls::handle_NtGetContextThread, this->second, buffer);
        auto& vcpu = this->emu->vcpu(1);
        this->wait_for_kick(vcpu, caller);

        const auto caller_context = this->context_buffer(0x800, CONTEXT_INTEGER_64, written_rax);
        const auto set_status = this->emu->dispatch_on_cpu(vcpu.cpu, [&] {
            return syscalls::handle_NtSetContextThread(this->context(1), this->first, caller_context); //
        });

        std::atomic_bool caller_returned{false};
        auto second_vcpu = this->run_second_vcpu_worker(caller_returned);
        const auto status = caller.get();
        caller_returned = true;

        EXPECT_EQ(STATUS_SUCCESS, set_status);
        EXPECT_EQ(STATUS_SUCCESS, status);
        EXPECT_TRUE(second_vcpu.get());
        EXPECT_EQ(second_rax, buffer.read().Rax);
        EXPECT_EQ(written_rax, this->rax(0));
        EXPECT_EQ(thread_ip, this->rip(0));
    }

    TEST_F(RunningThreadContextTest, ThreadInAnUnlockedHostCallContinuesAtAnIpSetMeanwhile)
    {
        constexpr uint64_t new_ip = 0x30000;
        const auto buffer = this->context_buffer(0, CONTEXT_CONTROL_64);
        auto value = buffer.read();
        value.Rip = new_ip;
        buffer.write(value);
        std::atomic_bool entered{false};
        std::atomic_bool released{false};
        auto host_call = this->unlocked_syscall_on_second_vcpu(entered, released);
        const auto release = utils::finally([&] { released = true; });
        this->wait_until(entered);

        const auto status = this->access_from_first_vcpu(syscalls::handle_NtSetContextThread, this->second, buffer).get();
        released = true;
        host_call.get();

        EXPECT_EQ(STATUS_SUCCESS, status);
        EXPECT_EQ(new_ip - this->emu->abi().syscall_abi().syscall_instruction_size(), this->rip(1));
        EXPECT_EQ(STATUS_SUCCESS, this->rax(1));
    }

    TEST_F(RunningThreadContextTest, WowDebugRegistersReachAThreadInAnUnlockedHostCall)
    {
        const auto reserved = this->give_wow64_context(this->second);
        const auto buffer = this->wow64_debug_register_write();
        std::atomic_bool entered{false};
        std::atomic_bool released{false};
        auto host_call = this->unlocked_host_wait_on_second_vcpu(entered, released);
        const auto release = utils::finally([&] { released = true; });
        this->wait_until(entered);

        const auto status = this->emu->dispatch_on_cpu(this->emu->vcpu(0).cpu, [&] {
            return syscalls::handle_NtSetInformationThread(this->context(0), this->second, ThreadWow64Context, buffer.value(),
                                                           sizeof(WOW64_CONTEXT));
        });
        released = true;
        host_call.get();

        EXPECT_EQ(STATUS_SUCCESS, status);
        EXPECT_EQ(0u, this->machine->second().stops.load());
        EXPECT_EQ(written_eax, reserved.read().Context.Eax);
        EXPECT_EQ(written_dr7, this->emu->vcpu(1).x64_cpu().reg(x86_register::dr7));
        EXPECT_EQ(second_rax, this->rax(1));
        EXPECT_EQ(first_rax, this->rax(0));
    }

    TEST_F(RunningThreadContextTest, WaitKeepsTheSavedMarkOfAnEnclosingUnlockedRegion)
    {
        std::atomic_bool caller_returned{false};
        auto second_vcpu = this->run_second_vcpu_worker(caller_returned);
        const auto [available, still_saved] = this->emu->dispatch_on_cpu(this->emu->vcpu(0).cpu, [&] {
            auto& vcpu = this->emu->vcpu(0);
            vcpu.active_thread_saved = true;
            const auto result = this->emu->wait_for_thread_registers(vcpu, this->thread(this->second));
            return std::pair{result, std::exchange(vcpu.active_thread_saved, false)};
        });
        caller_returned = true;

        EXPECT_TRUE(available);
        EXPECT_TRUE(still_saved);
        EXPECT_TRUE(second_vcpu.get());
    }
}

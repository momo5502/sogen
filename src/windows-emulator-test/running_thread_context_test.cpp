#include "emulation_test_utils.hpp"
#include <syscall_utils.hpp>

#include <cstring>
#include <future>
#include <map>
#include <span>

namespace sogen::syscalls
{
    NTSTATUS handle_NtGetContextThread(const syscall_context& c, handle thread_handle, emulator_object<CONTEXT64> thread_context);
    NTSTATUS handle_NtSetContextThread(const syscall_context& c, handle thread_handle, emulator_object<CONTEXT64> thread_context);
}

namespace sogen::test
{
    namespace
    {
        constexpr uint64_t stale_rax = 0xDEAD;
        constexpr uint64_t first_rax = 0x1001;
        constexpr uint64_t second_rax = 0x2001;
        constexpr uint64_t written_rax = 0x3001;

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

            void serialize_state(utils::buffer_serializer& /*buffer*/, bool /*is_snapshot*/) const override
            {
                throw std::runtime_error("Not supported");
            }

            void deserialize_state(utils::buffer_deserializer& /*buffer*/, bool /*is_snapshot*/) override
            {
                throw std::runtime_error("Not supported");
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

            void map_mmio(uint64_t /*address*/, size_t /*size*/, mmio_read_callback /*read_cb*/, mmio_write_callback /*write_cb*/) override
            {
                throw std::runtime_error("Not supported");
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
                this->first = this->run_thread(this->emu->vcpu(0), first_rax);
                this->second = this->run_thread(this->emu->vcpu(1), second_rax);
                this->storage = this->emu->memory.allocate_memory(0x1000, memory_permission::read_write);
            }

            handle run_thread(vcpu_context& vcpu, const uint64_t live_rax)
            {
                const auto thread_handle =
                    this->emu->process.threads.store(emulator_thread{this->emu->memory, this->emu->process, 0, 0, 0x100000, 0, 8, true});
                auto* thread = this->emu->process.threads.get(thread_handle);
                thread->setup_if_necessary(vcpu.cpu, this->emu->process);
                vcpu.x64_cpu().reg(x86_register::rip, 0x10000);
                vcpu.x64_cpu().reg(x86_register::rax, stale_rax);
                thread->save(vcpu.cpu);
                vcpu.x64_cpu().reg(x86_register::rax, live_rax);
                vcpu.active_thread = thread;
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

            std::unique_ptr<windows_emulator> emu{};
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

        const auto finished = first_vcpu.wait_for(std::chrono::seconds(10));
        if (finished != std::future_status::ready)
        {
            this->emu->stop();
        }

        EXPECT_EQ(std::future_status::ready, finished);
        EXPECT_EQ(STATUS_SUCCESS, first_vcpu.get());
        EXPECT_TRUE(second_vcpu.get());
        EXPECT_EQ(second_rax, first_buffer.read().Rax);
        EXPECT_EQ(first_rax, second_buffer.read().Rax);
        EXPECT_EQ(&this->thread(this->second), this->emu->vcpu(1).active_thread);
        EXPECT_EQ(second_rax, this->rax(1));
        EXPECT_EQ(first_rax, this->rax(0));
    }
}

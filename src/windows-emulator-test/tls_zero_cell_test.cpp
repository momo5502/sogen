#include "std_include.hpp"

namespace sogen::syscalls
{
    NTSTATUS handle_NtSetInformationThread(const syscall_context& c, handle thread_handle, THREADINFOCLASS info_class,
                                           uint64_t thread_information, uint32_t thread_information_length);
}

namespace sogen::test
{
    namespace
    {
        constexpr uint64_t untouched_slot = 0x1111111111111111;

        // ThreadZeroTlsCell clears one TLS cell in every thread of the process: cells below TLS_MINIMUM_AVAILABLE live in
        // the TEB's TlsSlots, the others at index cell - TLS_MINIMUM_AVAILABLE of the TlsExpansionSlots array.
        class TlsZeroCellTest : public testing::Test
        {
          protected:
            void SetUp() override
            {
                emulator_interfaces interfaces{};
                interfaces.audio = std::make_unique<null_audio_backend>();
                interfaces.console = std::make_unique<null_console_backend>();
                interfaces.ui = std::make_unique<null_ui_backend>();
                this->emu =
                    std::make_unique<windows_emulator>(create_x86_64_emulator_from_environment(),
                                                       emulator_settings{.disable_logging = true, .emulation_root = get_emulator_root()},
                                                       emulator_callbacks{}, std::move(interfaces));
                this->thread_handle =
                    this->emu->process.threads.store(emulator_thread{this->emu->memory, this->emu->process, 0, 0, 0x100000, 0, 8, true});
                this->thread = this->emu->process.threads.get(this->thread_handle);
                this->emu->vcpu(0).active_thread = this->thread;

                const auto storage = this->emu->memory.allocate_memory(0x10000, memory_permission::read_write);
                this->allocator.emplace(this->emu->memory, storage, 0x10000);
            }

            syscall_context context()
            {
                return {.win_emu = *this->emu, .emu = this->emu->vcpu(0).cpu, .vcpu = this->emu->vcpu(0), .proc = this->emu->process};
            }

            NTSTATUS zero_cell(const ULONG cell)
            {
                const auto argument = this->allocator->reserve<ULONG>();
                argument.write(cell);
                return syscalls::handle_NtSetInformationThread(this->context(), this->thread_handle, ThreadZeroTlsCell, argument.value(),
                                                               sizeof(ULONG));
            }

            template <typename Slot>
            uint64_t make_expansion_slots(emulator_allocator& slot_allocator)
            {
                const auto slots = slot_allocator.reserve(TLS_EXPANSION_SLOTS * sizeof(Slot), alignof(Slot));
                std::vector<Slot> values(TLS_EXPANSION_SLOTS, static_cast<Slot>(untouched_slot));
                this->emu->memory.write_memory(slots, values.data(), values.size() * sizeof(Slot));
                return slots;
            }

            template <typename Slot>
            Slot read_slot(const uint64_t slots, const size_t index)
            {
                Slot value{};
                this->emu->memory.read_memory(slots + index * sizeof(Slot), &value, sizeof(value));
                return value;
            }

            void write_pointer(const uint64_t address, const auto value)
            {
                this->emu->memory.write_memory(address, &value, sizeof(value));
            }

            std::unique_ptr<windows_emulator> emu{};
            handle thread_handle{};
            emulator_thread* thread{};
            std::optional<emulator_allocator> allocator{};
        };

        TEST_F(TlsZeroCellTest, ExpansionCellsClearTheirOwnSlot)
        {
            const auto slots = this->make_expansion_slots<uint64_t>(*this->allocator);
            this->write_pointer(this->thread->teb64->value() + offsetof(TEB64, TlsExpansionSlots), slots);

            ASSERT_EQ(STATUS_SUCCESS, this->zero_cell(TLS_MINIMUM_AVAILABLE));
            ASSERT_EQ(STATUS_SUCCESS, this->zero_cell(TLS_MINIMUM_AVAILABLE + TLS_EXPANSION_SLOTS - 1));

            EXPECT_EQ(this->read_slot<uint64_t>(slots, 0), 0u);
            EXPECT_EQ(this->read_slot<uint64_t>(slots, 1), untouched_slot);
            EXPECT_EQ(this->read_slot<uint64_t>(slots, TLS_EXPANSION_SLOTS - 2), untouched_slot);
            EXPECT_EQ(this->read_slot<uint64_t>(slots, TLS_EXPANSION_SLOTS - 1), 0u);
        }

        TEST_F(TlsZeroCellTest, Wow64ExpansionCellsClearTheirOwnSlot)
        {
            constexpr auto untouched_slot32 = static_cast<uint32_t>(untouched_slot);
            // A WoW64 TEB holds 32-bit pointers, so the slot array and the TEB itself live below 4 GiB.
            constexpr size_t low_size = 0x10000;
            const auto low = this->emu->memory.allocate_memory(low_size, memory_permission::read_write, false, 0x10000,
                                                               memory_region_kind::private_allocation, 0xFFFFFFFF);
            ASSERT_NE(low, 0u);
            ASSERT_LE(low + low_size, 0x100000000ull);
            emulator_allocator low_allocator{this->emu->memory, low, low_size};

            const auto slots = this->make_expansion_slots<uint32_t>(low_allocator);
            const auto teb32 = low_allocator.reserve<TEB32>();
            this->write_pointer(teb32.value() + offsetof(TEB32, TlsExpansionSlots), static_cast<uint32_t>(slots));
            this->thread->teb32 = teb32;
            this->emu->process.is_wow64_process = true;

            ASSERT_EQ(STATUS_SUCCESS, this->zero_cell(TLS_MINIMUM_AVAILABLE));
            ASSERT_EQ(STATUS_SUCCESS, this->zero_cell(TLS_MINIMUM_AVAILABLE + TLS_EXPANSION_SLOTS - 1));

            EXPECT_EQ(this->read_slot<uint32_t>(slots, 0), 0u);
            EXPECT_EQ(this->read_slot<uint32_t>(slots, 1), untouched_slot32);
            EXPECT_EQ(this->read_slot<uint32_t>(slots, TLS_EXPANSION_SLOTS - 2), untouched_slot32);
            EXPECT_EQ(this->read_slot<uint32_t>(slots, TLS_EXPANSION_SLOTS - 1), 0u);
        }
    }
}

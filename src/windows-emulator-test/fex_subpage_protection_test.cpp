#include "emulation_test_utils.hpp"

#ifdef __APPLE__
#include <memory_manager.hpp>
#include <utils/host_watchpoints.hpp>

#include <array>
#include <optional>
#include <vector>

namespace sogen::test
{
    namespace
    {
        constexpr size_t page_size = 0x1000;
        constexpr size_t host_page_size = 0x4000;
        constexpr uint64_t sentinel = 0x5A5A5A5A5A5A5A5AULL;
        constexpr uint64_t stored_value = 0x1122334455667788ULL;
        constexpr memory_permission guard_page = memory_permission::read_write | memory_permission::guard;

        struct observed_violation
        {
            uint64_t address{};
            memory_operation operation{};
            memory_violation_type type{};
        };

        enum class on_violation
        {
            stop,
            release_page,
        };

        class guest_code
        {
          public:
            guest_code& raw(const std::initializer_list<uint8_t> bytes)
            {
                this->bytes_.insert(this->bytes_.end(), bytes);
                return *this;
            }

            guest_code& disp32(const int32_t value)
            {
                for (size_t i = 0; i < sizeof(value); ++i)
                {
                    this->bytes_.push_back(static_cast<uint8_t>(static_cast<uint32_t>(value) >> (i * 8)));
                }
                return *this;
            }

            guest_code& store_rbx_at_rdi(const int32_t offset)
            {
                return this->raw({0x48, 0x89, 0x9F}).disp32(offset);
            }

            guest_code& load_rax_from_rdi(const int32_t offset)
            {
                return this->raw({0x48, 0x8B, 0x87}).disp32(offset);
            }

            guest_code& store_xmm0_at_rdi(const int32_t offset)
            {
                return this->raw({0xF3, 0x0F, 0x7F, 0x87}).disp32(offset);
            }

            guest_code& push_rbx()
            {
                return this->raw({0x53});
            }

            guest_code& sub_rsp(const int32_t value)
            {
                return this->raw({0x48, 0x81, 0xEC}).disp32(value);
            }

            std::vector<uint8_t> finish()
            {
                return this->raw({0xF4}).bytes_;
            }

          private:
            std::vector<uint8_t> bytes_{};
        };

        class FexSubpageProtectionTest : public testing::Test
        {
          protected:
            void SetUp() override
            {
                if (utils::host_watchpoints::capacity() == 0)
                {
                    GTEST_SKIP() << "Hardware watchpoints are not available on this host";
                }

                try
                {
                    this->emu_ = create_x86_64_emulator(backend_type::fex);
                }
                catch (const std::exception&)
                {
                    GTEST_SKIP() << "FEX backend is not available";
                }
            }

            void use_low_guest_window()
            {
                this->emu_->notify_process_bitness(true);
                this->fixed_base_ = 0x10000;
            }

            uint64_t allocate(const size_t size, const memory_permission permission)
            {
                auto& memory = this->memory();
                if (!this->fixed_base_)
                {
                    const uint64_t address = memory.allocate_memory(size, permission);
                    EXPECT_NE(address, 0u);
                    EXPECT_EQ(address % host_page_size, 0u);
                    return address;
                }

                const uint64_t address = *this->fixed_base_;
                EXPECT_TRUE(memory.allocate_memory(address, size, permission));
                *this->fixed_base_ += 0x10000;
                return address;
            }

            uint64_t allocate_filled(const size_t size)
            {
                const uint64_t address = this->allocate(size, memory_permission::read_write);
                const std::vector filler(size / sizeof(uint64_t), sentinel);
                this->memory().write_memory(address, filler.data(), size);
                return address;
            }

            void run(const std::vector<uint8_t>& code, const uint64_t rdi, const uint64_t rsp, const on_violation action)
            {
                auto& memory = this->memory();
                const uint64_t code_page = this->allocate(host_page_size, memory_permission::read_write);
                const uint64_t gdt = code_page + page_size;

                constexpr uint16_t long_mode_code_selector = 0x08;
                constexpr uint64_t long_mode_code_descriptor = 0x00AF9B000000FFFF;
                this->emu_->write_memory<uint64_t>(gdt + long_mode_code_selector, long_mode_code_descriptor);
                this->emu_->load_gdt(gdt, page_size);
                this->emu_->reg<uint16_t>(x86_register::cs, long_mode_code_selector);

                memory.write_memory(code_page, code.data(), code.size());
                ASSERT_TRUE(memory.protect_memory(code_page, page_size, memory_permission::read_exec));

                this->emu_->hook_memory_violation([this, action](cpu_interface& cpu, const uint64_t address, size_t,
                                                                 const memory_operation operation, const memory_violation_type type) {
                    this->violations_.push_back({.address = address, .operation = operation, .type = type});
                    switch (action)
                    {
                    case on_violation::release_page:
                        this->memory().protect_memory(address & ~(page_size - 1), page_size, memory_permission::read_write);
                        break;
                    case on_violation::stop:
                        cpu.stop();
                        break;
                    }
                    return memory_violation_continuation::resume;
                });
                this->emu_->hook_interrupt([](cpu_interface& cpu, int) { cpu.stop(); });

                this->emu_->reg(x86_register::rip, code_page);
                this->emu_->reg(x86_register::rdi, rdi);
                this->emu_->reg(x86_register::rsp, rsp);
                this->emu_->reg(x86_register::rax, sentinel);
                this->emu_->reg(x86_register::rbx, stored_value);
                this->emu_->start(0);
            }

            memory_manager& memory()
            {
                if (!this->memory_)
                {
                    this->memory_.emplace(*this->emu_);
                    this->memory_->reserve_host_memory_ranges();
                }
                return *this->memory_;
            }

            uint64_t read(const uint64_t address)
            {
                return this->memory().read_memory<uint64_t>(address);
            }

            std::unique_ptr<x86_64_emulator> emu_{};
            std::optional<memory_manager> memory_{};
            std::optional<uint64_t> fixed_base_{};
            std::vector<observed_violation> violations_{};
        };
    }

    TEST_F(FexSubpageProtectionTest, GuardPageBesideAccessiblePagesFaultsOnlyWhenTouched)
    {
        const uint64_t region = this->allocate_filled(host_page_size);
        ASSERT_TRUE(this->memory().protect_memory(region + 2 * page_size, page_size, guard_page));

        const auto code = guest_code{}
                              .store_rbx_at_rdi(0)
                              .store_rbx_at_rdi(3 * page_size)
                              .store_rbx_at_rdi(2 * page_size + 0x10)
                              .store_rbx_at_rdi(2 * page_size + 0x10)
                              .finish();
        this->run(code, region, region + host_page_size, on_violation::release_page);

        ASSERT_EQ(this->violations_.size(), 1u);
        EXPECT_EQ(this->violations_[0].address, region + 2 * page_size + 0x10);
        EXPECT_EQ(this->violations_[0].operation, memory_operation::write);
        EXPECT_EQ(this->violations_[0].type, memory_violation_type::protection);
        EXPECT_EQ(this->read(region), stored_value);
        EXPECT_EQ(this->read(region + page_size), sentinel);
        EXPECT_EQ(this->read(region + 2 * page_size + 0x10), stored_value);
        EXPECT_EQ(this->read(region + 3 * page_size), stored_value);
    }

    TEST_F(FexSubpageProtectionTest, ReadOfGuardPageBesideAccessiblePagesFaults)
    {
        const uint64_t region = this->allocate_filled(host_page_size);
        ASSERT_TRUE(this->memory().protect_memory(region + 2 * page_size, page_size, guard_page));

        const auto code = guest_code{}.load_rax_from_rdi(2 * page_size + 0x28).finish();
        this->run(code, region, region + host_page_size, on_violation::stop);

        ASSERT_EQ(this->violations_.size(), 1u);
        EXPECT_EQ(this->violations_[0].address, region + 2 * page_size + 0x28);
        EXPECT_EQ(this->violations_[0].operation, memory_operation::read);
        EXPECT_EQ(this->violations_[0].type, memory_violation_type::protection);
        EXPECT_EQ(this->emu_->reg(x86_register::rax), sentinel);
    }

    TEST_F(FexSubpageProtectionTest, StoreStraddlingIntoGuardPageReportsItsFirstByte)
    {
        const uint64_t region = this->allocate_filled(host_page_size);
        ASSERT_TRUE(this->memory().protect_memory(region + page_size, page_size, guard_page));

        const auto code = guest_code{}.store_xmm0_at_rdi(page_size - 8).finish();
        this->run(code, region, region + host_page_size, on_violation::stop);

        ASSERT_EQ(this->violations_.size(), 1u);
        EXPECT_EQ(this->violations_[0].address, region + page_size);
        EXPECT_EQ(this->violations_[0].operation, memory_operation::write);
        EXPECT_EQ(this->violations_[0].type, memory_violation_type::protection);
        EXPECT_EQ(this->read(region + page_size - 8), sentinel);
    }

    TEST_F(FexSubpageProtectionTest, GuardRegionBelowCommittedStackFaultsPageByPage)
    {
        constexpr size_t stack_size = 0x10000;
        const uint64_t stack = this->allocate_filled(stack_size);
        const uint64_t guard_region = stack + stack_size - 4 * page_size;
        ASSERT_TRUE(this->memory().protect_memory(guard_region, 3 * page_size, guard_page));

        const auto code =
            guest_code{}.push_rbx().sub_rsp(page_size).push_rbx().sub_rsp(page_size).push_rbx().sub_rsp(page_size).push_rbx().finish();
        this->run(code, 0, stack + stack_size, on_violation::release_page);

        const std::array expected_pushes{stack + 0xEFF0, stack + 0xDFE8, stack + 0xCFE0};
        ASSERT_EQ(this->violations_.size(), expected_pushes.size());
        for (size_t i = 0; i < expected_pushes.size(); ++i)
        {
            EXPECT_EQ(this->violations_[i].address, expected_pushes[i]);
            EXPECT_EQ(this->violations_[i].operation, memory_operation::write);
            EXPECT_EQ(this->violations_[i].type, memory_violation_type::protection);
            EXPECT_EQ(this->read(expected_pushes[i]), stored_value);
        }
        EXPECT_EQ(this->emu_->reg(x86_register::rsp), expected_pushes.back());
        EXPECT_EQ(this->read(guard_region - 8), sentinel);
    }

    TEST_F(FexSubpageProtectionTest, GuardPageInLowGuestWindowReportsTheGuestAddress)
    {
        this->use_low_guest_window();
        const uint64_t region = this->allocate_filled(host_page_size);
        ASSERT_TRUE(this->memory().protect_memory(region + 2 * page_size, page_size, guard_page));

        const auto code = guest_code{}.store_rbx_at_rdi(page_size).store_rbx_at_rdi(2 * page_size + 0x30).finish();
        this->run(code, region, region + host_page_size, on_violation::stop);

        ASSERT_EQ(this->violations_.size(), 1u);
        EXPECT_EQ(this->violations_[0].address, region + 2 * page_size + 0x30);
        EXPECT_EQ(this->violations_[0].operation, memory_operation::write);
        EXPECT_EQ(this->violations_[0].type, memory_violation_type::protection);
        EXPECT_EQ(this->read(region + page_size), stored_value);
    }
}
#endif

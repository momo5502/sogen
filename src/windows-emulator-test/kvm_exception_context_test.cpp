#include <kvm_exception_context.hpp>

#include <gtest/gtest.h>

#include <array>
#include <limits>

namespace sogen::kvm::test
{
    namespace
    {
        constexpr uint64_t frame_address = 0xffff800000004000;
        constexpr uint64_t trampoline_address = 0xffff800000001100;
        constexpr uint64_t native_dispatcher = 0x7ff000166310;

        class ExceptionContextTest : public testing::Test
        {
          protected:
            kvm_regs regs{};
            kvm_sregs sregs{};
            detail::exception_frame frame{
                .rip = 0x7006ec98,
                .cs = 0x23,
                .rflags = 0x202,
                .rsp = 0x2ff000,
                .ss = 0x2b,
            };

            ExceptionContextTest()
            {
                this->sregs.cs = detail::make_segment(0x33, true, false);
                this->sregs.cs.selector = 0x08;
                this->sregs.ss = detail::make_segment(0x10, false, false);
                this->sregs.ds = detail::make_segment(0x2b, false, true);
                this->sregs.es = this->sregs.ds;
                this->regs.rip = trampoline_address - 1;
                this->regs.rsp = frame_address;
            }

            template <typename Callback>
            detail::exception_dispatch_result dispatch(const uint32_t vector, Callback&& callback)
            {
                return detail::dispatch_exception_context(this->regs, this->sregs, this->frame, vector, frame_address, trampoline_address,
                                                          std::forward<Callback>(callback));
            }
        };
    }

    TEST(KvmSegmentTest, FlatSegmentsCoverTheEntire32BitAddressSpace)
    {
        constexpr uint32_t last_byte = std::numeric_limits<uint32_t>::max();
        const std::array segments{
            detail::make_segment(0x23, true, true),
            detail::make_segment(0x33, true, true),
            detail::make_segment(0x2b, false, true),
            detail::make_segment(0x10, false, false),
        };
        for (const auto& segment : segments)
        {
            SCOPED_TRACE(segment.selector);
            EXPECT_EQ(segment.base, 0);
            EXPECT_EQ(segment.limit, last_byte);
            EXPECT_EQ(segment.g, 1);
        }
    }

    TEST_F(ExceptionContextTest, CompatibilityFaultRestoresFullDataSegmentLimits)
    {
        this->sregs.ds.limit = 0xFFFFF;
        this->sregs.ds.g = 0;
        this->sregs.es = this->sregs.ds;
        const auto result = this->dispatch(13, [&] {
            constexpr uint32_t last_byte = std::numeric_limits<uint32_t>::max();
            EXPECT_EQ(this->sregs.cs.limit, last_byte);
            EXPECT_EQ(this->sregs.ss.limit, last_byte);
            EXPECT_EQ(this->sregs.ds.limit, last_byte);
            EXPECT_EQ(this->sregs.es.limit, last_byte);
            EXPECT_EQ(this->sregs.ds.g, 1);
            EXPECT_EQ(this->sregs.es.g, 1);
            return true;
        });
        EXPECT_TRUE(result.handled);
        EXPECT_TRUE(result.return_frame);
    }

    TEST_F(ExceptionContextTest, CompatibilityFaultCallbackObservesTheFaultingUserContext)
    {
        const auto result = this->dispatch(13, [&] {
            EXPECT_EQ(this->regs.rip, this->frame.rip);
            EXPECT_EQ(this->regs.rsp, this->frame.rsp);
            EXPECT_EQ(this->regs.rflags, this->frame.rflags);
            EXPECT_EQ(this->sregs.cs.selector, 0x23);
            EXPECT_EQ(this->sregs.cs.l, 0);
            EXPECT_EQ(this->sregs.cs.db, 1);
            EXPECT_EQ(this->sregs.cs.dpl, 3);
            EXPECT_EQ(this->sregs.ss.selector, 0x2b);
            EXPECT_EQ(this->sregs.ss.dpl, 3);
            return true;
        });
        ASSERT_TRUE(result.handled);
        ASSERT_TRUE(result.return_frame);
        EXPECT_EQ(result.return_frame->rip, this->frame.rip);
        EXPECT_EQ(result.return_frame->cs, 0x23);
        EXPECT_EQ(this->sregs.cs.selector, 0x08);
        EXPECT_EQ(this->sregs.cs.l, 1);
        EXPECT_EQ(this->sregs.ss.selector, 0x10);
        EXPECT_EQ(this->regs.rip, trampoline_address);
        EXPECT_EQ(this->regs.rsp, frame_address);
    }

    TEST_F(ExceptionContextTest, CompatibilityCallbackCanRedirectToNativeCode)
    {
        const auto result = this->dispatch(13, [&] {
            this->regs.rip = native_dispatcher;
            this->regs.rsp = 0x7ff000800000;
            this->sregs.cs.selector = 0x33;
            this->sregs.ss.selector = 0x2b;
            return true;
        });
        ASSERT_TRUE(result.return_frame);
        EXPECT_EQ(result.return_frame->rip, native_dispatcher);
        EXPECT_EQ(result.return_frame->rsp, 0x7ff000800000);
        EXPECT_EQ(result.return_frame->cs, 0x33);
        EXPECT_EQ(result.return_frame->ss, 0x2b);
        EXPECT_EQ(this->sregs.cs.selector, 0x08);
    }

    TEST_F(ExceptionContextTest, NativeCallbackKeepsItsAdjustedContext)
    {
        this->frame.cs = 0x33;
        this->frame.rip = 0x7ff000100000;
        const auto result = this->dispatch(14, [&] {
            EXPECT_EQ(this->sregs.cs.selector, 0x33);
            EXPECT_EQ(this->sregs.cs.l, 1);
            this->regs.rip = native_dispatcher;
            this->regs.rsp = 0x7ff000800000;
            return true;
        });
        EXPECT_TRUE(result.handled);
        EXPECT_FALSE(result.return_frame);
        EXPECT_EQ(this->regs.rip, native_dispatcher);
        EXPECT_EQ(this->regs.rsp, 0x7ff000800000);
        EXPECT_EQ(this->sregs.cs.selector, 0x33);
    }

    TEST_F(ExceptionContextTest, NativeCallbackCanReturnThroughACompatibilityFrame)
    {
        this->frame.cs = 0x33;
        const auto result = this->dispatch(14, [&] {
            this->regs.rip = 0x401000;
            this->sregs.cs.selector = 0x23;
            return true;
        });
        ASSERT_TRUE(result.return_frame);
        EXPECT_EQ(result.return_frame->rip, 0x401000);
        EXPECT_EQ(result.return_frame->cs, 0x23);
        EXPECT_EQ(this->sregs.cs.selector, 0x08);
    }

    TEST_F(ExceptionContextTest, BreakpointRewindsTheFaultAddressAndPreservesTheGuestTrapFlag)
    {
        this->frame.rflags |= 1ULL << 8;
        const auto result = this->dispatch(3, [&] {
            EXPECT_EQ(this->regs.rip, this->frame.rip - 1);
            return true;
        });
        ASSERT_TRUE(result.return_frame);
        EXPECT_EQ(result.return_frame->rip, this->frame.rip - 1);
        EXPECT_EQ(result.return_frame->rflags, this->frame.rflags);
        EXPECT_EQ(this->regs.rflags & (1ULL << 8), 0);
    }

    TEST_F(ExceptionContextTest, UnhandledFaultDoesNotScheduleATrampoline)
    {
        const auto result = this->dispatch(13, [] { return false; });
        EXPECT_FALSE(result.handled);
        EXPECT_FALSE(result.return_frame);
        EXPECT_EQ(this->regs.rip, this->frame.rip);
    }
}

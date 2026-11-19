#include "std_include.hpp"

namespace sogen::syscalls
{
    BOOL handle_NtUserReplyMessage(const syscall_context& c, uint64_t result);
}

namespace sogen::test
{
    namespace
    {
        class SentWindowMessageTest : public testing::Test
        {
          protected:
            emulator_thread& create_thread(const uint32_t id)
            {
                const auto handle =
                    this->emu.process.threads.store(emulator_thread{this->emu.memory, this->emu.process, 0, 0, 0, 0, id, false});
                return *this->emu.process.threads.get(handle);
            }

            windows_emulator emu = create_empty_emulator();
        };
    }

    TEST_F(SentWindowMessageTest, WaitingSenderCanServiceAnIncomingSend)
    {
        auto& sender = this->create_thread(8);
        sender.sent_message_waits.emplace_back();
        EXPECT_FALSE(sender.is_thread_ready(this->emu));

        sender.pending_sent_messages.push_back({.sender_thread_id = 12});
        EXPECT_TRUE(sender.is_thread_ready(this->emu));

        sender.callback_stack.emplace_back();
        EXPECT_EQ(nullptr, sender.current_sent_message_wait());
        sender.sent_message_waits.push_back({.callback_depth = 1});
        sent_window_message outer{.sender_thread_id = sender.id};
        sent_window_message inner{.sender_thread_id = sender.id, .sender_callback_depth = 1};
        this->emu.process.reply_to_sent_message(outer, 41);
        EXPECT_FALSE(sender.current_sent_message_wait()->result.has_value());
        this->emu.process.reply_to_sent_message(inner, 42);
        EXPECT_EQ(42u, sender.current_sent_message_wait()->result);
        sender.sent_message_waits.pop_back();
        sender.callback_stack.pop_back();
        EXPECT_EQ(41u, sender.current_sent_message_wait()->result);
    }

    TEST_F(SentWindowMessageTest, ReceiverExitReleasesQueuedAndActiveSenders)
    {
        auto& queued = this->create_thread(8);
        auto& active = this->create_thread(12);
        auto& replied = this->create_thread(16);
        auto& receiver = this->create_thread(20);
        queued.sent_message_waits.emplace_back();
        active.sent_message_waits.emplace_back();
        replied.sent_message_waits.emplace_back();

        receiver.pending_sent_messages.push_back({.sender_thread_id = queued.id});
        receiver.active_sent_messages.push_back({.sender_thread_id = active.id});
        receiver.active_sent_messages.push_back({.sender_thread_id = replied.id});
        this->emu.process.reply_to_sent_message(receiver.active_sent_messages.back(), 23);
        this->emu.process.terminate_thread(receiver, STATUS_SUCCESS);

        EXPECT_TRUE(queued.is_thread_ready(this->emu));
        EXPECT_TRUE(active.is_thread_ready(this->emu));
        EXPECT_EQ(0u, queued.current_sent_message_wait()->result);
        EXPECT_EQ(0u, active.current_sent_message_wait()->result);
        EXPECT_EQ(23u, replied.current_sent_message_wait()->result);
    }

    TEST_F(SentWindowMessageTest, ReplyMessageOnlySucceedsOnceForActiveSend)
    {
        auto& receiver = this->create_thread(8);
        auto& sender = this->create_thread(12);
        this->emu.vcpu(0).set_active_thread(&receiver);
        sender.sent_message_waits.emplace_back();
        receiver.active_sent_messages.push_back({.sender_thread_id = sender.id});
        const syscall_context c{.win_emu = this->emu, .emu = this->emu.vcpu(0).cpu, .vcpu = this->emu.vcpu(0), .proc = this->emu.process};

        EXPECT_EQ(TRUE, syscalls::handle_NtUserReplyMessage(c, 123));
        ASSERT_TRUE(receiver.active_sent_messages.back().replied);
        ASSERT_EQ(123u, sender.current_sent_message_wait()->result);

        EXPECT_EQ(FALSE, syscalls::handle_NtUserReplyMessage(c, 456));
        EXPECT_EQ(123u, sender.current_sent_message_wait()->result);
    }
}

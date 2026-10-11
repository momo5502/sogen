#include <utils/host_pipe.hpp>
#include <gtest/gtest.h>

#include <chrono>
#include <stdexcept>
#include <thread>
#include <vector>

namespace sogen::test
{
    namespace
    {
        utils::pipe_io_result finish(utils::host_pipe_operation& operation)
        {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            auto result = operation.check_completion();
            while (result.status == utils::pipe_io_status::pending && std::chrono::steady_clock::now() < deadline)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                result = operation.check_completion();
            }
            return result;
        }

        class HostPipeTest : public testing::TestWithParam<bool>
        {
          protected:
            void SetUp() override
            {
                auto pair = utils::host_pipe::create_pair(false, 4);
                this->reader = this->endpoint(std::move(pair.first));
                this->writer = this->endpoint(std::move(pair.second));
            }

            std::shared_ptr<utils::host_pipe> endpoint(std::shared_ptr<utils::host_pipe> pipe)
            {
                if (!GetParam())
                {
                    return pipe;
                }
                auto transfer = pipe->export_endpoint();
                auto remote = utils::host_pipe::connect(transfer.port(), transfer.token());
                this->transfers.push_back(std::move(transfer));
                return remote;
            }

            std::vector<utils::host_pipe_transfer> transfers;
            std::shared_ptr<utils::host_pipe> reader;
            std::shared_ptr<utils::host_pipe> writer;
        };
    }

    TEST_P(HostPipeTest, PendingReadCompletesAfterWrite)
    {
        auto read = this->reader->read(16);
        EXPECT_EQ(read->check_completion().status, utils::pipe_io_status::pending);
        auto write = this->writer->write("abc");
        ASSERT_EQ(finish(*write).status, utils::pipe_io_status::success);
        ASSERT_EQ(finish(*read).status, utils::pipe_io_status::success);
        EXPECT_EQ(read->data(), "abc");
    }

    TEST_P(HostPipeTest, BackpressurePreservesPartialWriteProgress)
    {
        auto write = this->writer->write("abcdef");
        const auto pending = write->check_completion();
        EXPECT_EQ(pending.status, utils::pipe_io_status::pending);
        EXPECT_EQ(pending.transferred, 4u);
        auto first = this->reader->read(2);
        ASSERT_EQ(finish(*first).status, utils::pipe_io_status::success);
        EXPECT_EQ(first->data(), "ab");
        const auto completed = finish(*write);
        EXPECT_EQ(completed.status, utils::pipe_io_status::success);
        EXPECT_EQ(completed.transferred, 6u);
        auto remaining = this->reader->read(16);
        ASSERT_EQ(finish(*remaining).status, utils::pipe_io_status::success);
        EXPECT_EQ(remaining->data(), "cdef");
    }

    TEST_P(HostPipeTest, PeekReportsAvailableBytesWithoutConsumingThem)
    {
        auto write = this->writer->write("abcd");
        ASSERT_EQ(finish(*write).status, utils::pipe_io_status::success);
        std::string data;
        uint32_t available{};
        EXPECT_EQ(this->reader->peek(data, 2, available).status, utils::pipe_io_status::success);
        EXPECT_EQ(data, "ab");
        EXPECT_EQ(available, 4u);
        auto read = this->reader->read(4);
        ASSERT_EQ(finish(*read).status, utils::pipe_io_status::success);
        EXPECT_EQ(read->data(), "abcd");
    }

    TEST_P(HostPipeTest, DuplicateKeepsPeerAliveUntilItsLastReferenceCloses)
    {
        auto duplicate = this->writer->duplicate();
        auto read = this->reader->read(16);
        this->writer.reset();
        EXPECT_EQ(read->check_completion().status, utils::pipe_io_status::pending);
        duplicate.reset();
        EXPECT_EQ(finish(*read).status, utils::pipe_io_status::broken);
    }

    TEST_P(HostPipeTest, BufferedDataDrainsBeforeEof)
    {
        auto write = this->writer->write("data");
        ASSERT_EQ(finish(*write).status, utils::pipe_io_status::success);
        write.reset();
        this->writer.reset();
        auto read = this->reader->read(16);
        ASSERT_EQ(finish(*read).status, utils::pipe_io_status::success);
        EXPECT_EQ(read->data(), "data");
        auto eof = this->reader->read(16);
        EXPECT_EQ(finish(*eof).status, utils::pipe_io_status::broken);
    }

    TEST_P(HostPipeTest, CancelledReadDoesNotConsumeTheNextWrite)
    {
        auto read = this->reader->read(16);
        ASSERT_EQ(read->check_completion().status, utils::pipe_io_status::pending);
        read->cancel();
        EXPECT_EQ(read->check_completion().status, utils::pipe_io_status::cancelled);
        auto write = this->writer->write("next");
        ASSERT_EQ(finish(*write).status, utils::pipe_io_status::success);
        auto next = this->reader->read(16);
        ASSERT_EQ(finish(*next).status, utils::pipe_io_status::success);
        EXPECT_EQ(next->data(), "next");
    }

    TEST_P(HostPipeTest, ClosedReaderBreaksWrites)
    {
        this->reader.reset();
        auto write = this->writer->write("data");
        EXPECT_EQ(finish(*write).status, utils::pipe_io_status::broken);
    }

    TEST_P(HostPipeTest, TransferredEndpointCanBeInheritedAgain)
    {
        auto transfer = this->writer->export_endpoint();
        auto grandchild = utils::host_pipe::connect(transfer.port(), transfer.token());
        this->writer.reset();
        auto write = grandchild->write("data");
        ASSERT_EQ(finish(*write).status, utils::pipe_io_status::success);
        auto read = this->reader->read(16);
        ASSERT_EQ(finish(*read).status, utils::pipe_io_status::success);
        EXPECT_EQ(read->data(), "data");
        write.reset();
        grandchild.reset();
        auto eof = this->reader->read(16);
        EXPECT_EQ(finish(*eof).status, utils::pipe_io_status::broken);
    }

    TEST_P(HostPipeTest, LargeWritesSurviveBackpressureAndTransportFragmentation)
    {
        auto pair = utils::host_pipe::create_pair(false, 65536);
        auto large_reader = this->endpoint(std::move(pair.first));
        auto large_writer = this->endpoint(std::move(pair.second));
        std::string input(192 * 1024, '\0');
        for (size_t i = 0; i < input.size(); ++i)
        {
            input[i] = static_cast<char>(i % 251);
        }
        auto write = large_writer->write(input);
        std::string output;
        while (output.size() < input.size())
        {
            const auto progress = write->check_completion();
            ASSERT_TRUE(progress.status == utils::pipe_io_status::pending || progress.status == utils::pipe_io_status::success);
            auto read = large_reader->read(16381);
            ASSERT_EQ(finish(*read).status, utils::pipe_io_status::success);
            ASSERT_FALSE(read->data().empty());
            output.append(read->data());
        }
        EXPECT_EQ(finish(*write).status, utils::pipe_io_status::success);
        EXPECT_EQ(output, input);
    }

    TEST_P(HostPipeTest, CancellingPartialWriteRetainsOnlyAlreadyTransferredBytes)
    {
        auto write = this->writer->write("abcdef");
        ASSERT_EQ(write->check_completion().status, utils::pipe_io_status::pending);
        write->cancel();
        const auto cancelled = write->check_completion();
        EXPECT_EQ(cancelled.status, utils::pipe_io_status::cancelled);
        EXPECT_EQ(cancelled.transferred, 4u);
        auto read = this->reader->read(16);
        ASSERT_EQ(finish(*read).status, utils::pipe_io_status::success);
        EXPECT_EQ(read->data(), "abcd");
        auto next = this->reader->read(16);
        EXPECT_EQ(next->check_completion().status, utils::pipe_io_status::pending);
    }

    TEST_P(HostPipeTest, MessagePipeSupportsByteReadsOnBothTransports)
    {
        auto pair = utils::host_pipe::create_pair(true, 4);
        auto message_reader = this->endpoint(std::move(pair.first));
        auto message_writer = this->endpoint(std::move(pair.second));
        auto write = message_writer->write("message");
        ASSERT_EQ(finish(*write).status, utils::pipe_io_status::success);
        auto first = message_reader->read(4);
        ASSERT_EQ(finish(*first).status, utils::pipe_io_status::success);
        EXPECT_EQ(first->data(), "mess");
        auto remaining = message_reader->read(4);
        ASSERT_EQ(finish(*remaining).status, utils::pipe_io_status::success);
        EXPECT_EQ(remaining->data(), "age");
    }

    TEST(HostPipeTransferTest, InvalidAuthenticationDoesNotConsumeTheTransfer)
    {
        auto pair = utils::host_pipe::create_pair(false, 4);
        auto transfer = pair.second->export_endpoint();
        auto invalid = transfer.token();
        invalid.front() = invalid.front() == '0' ? '1' : '0';
        EXPECT_THROW(utils::host_pipe::connect(transfer.port(), invalid), std::runtime_error);
        auto remote = utils::host_pipe::connect(transfer.port(), transfer.token());
        auto write = remote->write("data");
        ASSERT_EQ(finish(*write).status, utils::pipe_io_status::success);
        auto read = pair.first->read(16);
        ASSERT_EQ(finish(*read).status, utils::pipe_io_status::success);
        EXPECT_EQ(read->data(), "data");
    }

    TEST(HostPipeTransferTest, StoppingTransferDisconnectsIdleClient)
    {
        auto pair = utils::host_pipe::create_pair(false, 4);
        std::shared_ptr<utils::host_pipe> remote;
        {
            auto transfer = pair.second->export_endpoint();
            remote = utils::host_pipe::connect(transfer.port(), transfer.token());
        }
        auto write = remote->write("data");
        EXPECT_EQ(finish(*write).status, utils::pipe_io_status::broken);
    }

    TEST(HostPipeTransferTest, UnclaimedTransferCanBeDestroyed)
    {
        auto pair = utils::host_pipe::create_pair(false, 4);
        {
            auto transfer = pair.second->export_endpoint();
            pair.second.reset();
        }
        auto read = pair.first->read(1);
        EXPECT_EQ(finish(*read).status, utils::pipe_io_status::broken);
    }

    INSTANTIATE_TEST_SUITE_P(LocalAndTransferred, HostPipeTest, testing::Bool());
}

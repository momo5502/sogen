#include <gtest/gtest.h>

#include <adaptive_mutex.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

#ifdef _MSC_VER
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace sogen::test
{
    using namespace std::chrono_literals;

    namespace
    {
        class AdaptiveMutexTest : public testing::Test
        {
          protected:
            void SetUp() override
            {
#if defined(__EMSCRIPTEN__) && !defined(__EMSCRIPTEN_PTHREADS__)
                GTEST_SKIP() << "Requires host threads";
#endif
            }
        };
    }

    TEST_F(AdaptiveMutexTest, KeepsOneHolderAtATime)
    {
        adaptive_mutex lock{};
        uint64_t counter = 0; // only the holder touches it
        constexpr uint64_t threads = 8;
        constexpr uint64_t rounds = 20000;

        std::vector<std::thread> workers{};
        for (uint64_t i = 0; i < threads; ++i)
        {
            workers.emplace_back([&] {
                for (uint64_t round = 0; round < rounds; ++round)
                {
                    const std::scoped_lock guard(lock);
                    ++counter;
                }
            });
        }
        for (auto& worker : workers)
        {
            worker.join();
        }

        EXPECT_EQ(counter, threads * rounds);
    }

    TEST_F(AdaptiveMutexTest, TryLockOnlyTakesAFreeLock)
    {
        adaptive_mutex lock{};
        ASSERT_TRUE(lock.try_lock());

        bool taken_while_held = true;
        std::thread([&] { taken_while_held = lock.try_lock(); }).join();
        EXPECT_FALSE(taken_while_held);

        lock.unlock();
        EXPECT_TRUE(lock.try_lock());
        lock.unlock();
    }

    TEST_F(AdaptiveMutexTest, WakesEveryParkedWaiter)
    {
        adaptive_mutex lock{};
        lock.lock();

        std::atomic<int> acquired{0};
        std::vector<std::thread> waiters{};
        for (int i = 0; i < 6; ++i)
        {
            waiters.emplace_back([&] {
                const std::scoped_lock guard(lock);
                acquired.fetch_add(1);
            });
        }

        std::this_thread::sleep_for(50ms); // long past the time a waiter parks
        EXPECT_EQ(acquired.load(), 0);

        lock.unlock();
        for (auto& waiter : waiters)
        {
            waiter.join();
        }
        EXPECT_EQ(acquired.load(), 6);
    }

    TEST_F(AdaptiveMutexTest, WorksWithConditionVariableAny)
    {
        adaptive_mutex lock{};
        std::condition_variable_any changed{};
        bool ready = false;

        std::thread signaller([&] {
            std::this_thread::sleep_for(10ms);
            {
                const std::scoped_lock guard(lock);
                ready = true;
            }
            changed.notify_all();
        });

        {
            std::unique_lock guard(lock);
            EXPECT_TRUE(changed.wait_for(guard, 5s, [&] { return ready; }));
        }
        signaller.join();
    }

#ifdef _MSC_VER
    // A lock that reserves itself for the next waiter in line stalls every other thread while that waiter is not
    // running, as when other work takes the host's cores.
    //
    // MSVC only: MinGW's libstdc++ waits and notifies under a shared mutex, so suspending the waiter while it holds
    // that mutex blocks unlock() in notify_one() forever.
    TEST_F(AdaptiveMutexTest, AWaiterThatIsNotRunningDoesNotKeepTheLockFromOthers)
    {
        for (int round = 0; round < 20; ++round)
        {
            adaptive_mutex lock{};
            lock.lock();

            std::atomic<bool> asking{false};
            std::thread waiter([&] {
                asking = true;
                const std::scoped_lock guard(lock);
            });
            while (!asking)
            {
                std::this_thread::yield();
            }

            const HANDLE waiter_handle = waiter.native_handle();
            ASSERT_NE(SuspendThread(waiter_handle), static_cast<DWORD>(-1));
            lock.unlock();

            bool taken = false;
            std::thread other([&] {
                const auto deadline = std::chrono::steady_clock::now() + 10s;
                while (!taken && std::chrono::steady_clock::now() < deadline)
                {
                    taken = lock.try_lock();
                }
                if (taken)
                {
                    lock.unlock();
                }
            });
            other.join();

            ASSERT_NE(ResumeThread(waiter_handle), static_cast<DWORD>(-1));
            waiter.join();
            ASSERT_TRUE(taken) << "round " << round;
        }
    }
#endif
}

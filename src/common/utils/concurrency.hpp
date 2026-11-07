#pragma once

#include <mutex>

#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86) || defined(_M_ARM64))
#include <intrin.h>
#endif

namespace sogen
{

    namespace utils::concurrency
    {
        // The processor's spin-loop hint: it eases a busy wait without giving up the running thread, unlike
        // std::this_thread::yield.
        inline void cpu_relax()
        {
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
            _mm_pause();
#elif defined(_MSC_VER) && defined(_M_ARM64)
            __yield();
#elif defined(__x86_64__) || defined(__i386__)
            __builtin_ia32_pause();
#elif defined(__aarch64__) || defined(__arm__)
            asm volatile("yield");
#elif defined(__wasm__)
            // WebAssembly has no spin-loop hint instruction, so a spin here only re-reads.
#else
#error "cpu_relax needs this architecture's spin-loop hint"
#endif
        }

        template <typename T, typename MutexType = std::mutex>
        class container
        {
          public:
            template <typename R = void, typename F>
            R access(F&& accessor) const
            {
                std::lock_guard<MutexType> _{mutex_};
                return accessor(object_);
            }

            template <typename R = void, typename F>
            R access(F&& accessor)
            {
                std::lock_guard<MutexType> _{mutex_};
                return accessor(object_);
            }

            template <typename R = void, typename F>
            R access_with_lock(F&& accessor) const
            {
                std::unique_lock<MutexType> lock{mutex_};
                return accessor(object_, lock);
            }

            template <typename R = void, typename F>
            R access_with_lock(F&& accessor)
            {
                std::unique_lock<MutexType> lock{mutex_};
                return accessor(object_, lock);
            }

            T& get_raw()
            {
                return object_;
            }

            const T& get_raw() const
            {
                return object_;
            }

            T copy() const
            {
                std::unique_lock<MutexType> lock{mutex_};
                return object_;
            }

            std::unique_lock<MutexType> acquire_lock()
            {
                return std::unique_lock<MutexType>{mutex_};
            }

          private:
            mutable MutexType mutex_{};
            T object_{};
        };
    }
} // namespace sogen

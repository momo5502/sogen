#pragma once

#include "std_include.hpp"
#include <serialization.hpp>

#include "arch_emulator.hpp"

#include <mutex>
#include <utils/time.hpp>

namespace sogen
{

    struct process_context;
    struct fake_environment_config;
    class windows_emulator;
    class windows_version_manager;

    class kusd_mmio
    {
      public:
        kusd_mmio(memory_manager& memory, utils::clock& clock);
        ~kusd_mmio();

        kusd_mmio(utils::buffer_deserializer& buffer);

        kusd_mmio(kusd_mmio&&) = delete;
        kusd_mmio(const kusd_mmio&) = delete;
        kusd_mmio& operator=(kusd_mmio&& obj) = delete;
        kusd_mmio& operator=(const kusd_mmio&) = delete;

        void serialize(utils::buffer_serializer& buffer) const;
        void deserialize(utils::buffer_deserializer& buffer);

        // Locked access to the KUSD block: the MMIO read callback mutates kusd_ under mutex_ on the WHP
        // worker thread, so callers must not touch it unsynchronized. The functor's result is forwarded.
        template <typename F>
        decltype(auto) access(const F& functor)
        {
            const std::lock_guard lock{this->mutex_};
            return functor(this->kusd_);
        }

        template <typename F>
        decltype(auto) access(const F& functor) const
        {
            const std::lock_guard lock{this->mutex_};
            return functor(this->kusd_);
        }

        static uint64_t address();

        void setup(const windows_version_manager& version, const fake_environment_config& fake_env, bool is_wow64_process);

      private:
        memory_manager* memory_{};
        utils::clock* clock_{};

        bool registered_{};

        // The MMIO read callback runs on the WHP worker thread during guest execution
        // (outside the kernel lock), so multiple vCPUs can read KUSD concurrently. This
        // guards update() + the read snapshot so kusd_ is never torn.
        mutable std::mutex mutex_{};

        // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization)
        KUSER_SHARED_DATA64 kusd_{};

        void read(uint64_t addr, void* data, size_t size);

        void update();

        void register_mmio();
        void deregister_mmio();
    };

} // namespace sogen

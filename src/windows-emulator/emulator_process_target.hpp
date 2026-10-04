#pragma once

#include "windows_emulator.hpp"

#include <process_manager.hpp>

#include <limits>

namespace sogen
{
    class emulator_process_target final : public managed_process_target
    {
      public:
        explicit emulator_process_target(windows_emulator& emulator)
            : emulator_(emulator)
        {
        }

        uint64_t native_environment() const override
        {
            return this->emulator_.process.peb64.value();
        }

        uint64_t compatibility_environment() const override
        {
            return this->emulator_.process.peb32 ? this->emulator_.process.peb32->value() : 0;
        }

        uint64_t native_parameters() const override
        {
            return this->emulator_.process.process_params64.value();
        }

        uint64_t compatibility_parameters() const override
        {
            return this->emulator_.process.process_params32 ? this->emulator_.process.process_params32->value() : 0;
        }

        process_memory_result allocate_memory(const uint64_t address, const uint64_t size, const memory_permission permission,
                                              const bool reserve, const bool commit) override
        {
            return this->emulator_.synchronize([&] {
                if (size > std::numeric_limits<size_t>::max())
                {
                    return process_memory_result{.error = process_error::resource_limit};
                }

                const nt_memory_permission emulator_permission{permission};
                uint64_t result = address;
                if (reserve)
                {
                    result = address
                                 ? (this->emulator_.memory.allocate_memory(address, static_cast<size_t>(size), emulator_permission, !commit)
                                        ? address
                                        : 0)
                                 : this->emulator_.memory.allocate_memory(static_cast<size_t>(size), emulator_permission, !commit);
                }
                else if (commit && address && this->emulator_.memory.commit_memory(address, static_cast<size_t>(size), emulator_permission))
                {
                    result = address;
                }
                else if (commit)
                {
                    result =
                        address ? (this->emulator_.memory.allocate_memory(address, static_cast<size_t>(size), emulator_permission) ? address
                                                                                                                                   : 0)
                                : this->emulator_.memory.allocate_memory(static_cast<size_t>(size), emulator_permission);
                }
                else
                {
                    result = 0;
                }

                return result ? process_memory_result{.address = result, .size = size}
                              : process_memory_result{.error = process_error::unavailable};
            });
        }

        process_error free_memory(const uint64_t address, const uint64_t size, const bool release) override
        {
            return this->emulator_.synchronize([&] {
                if (size > std::numeric_limits<size_t>::max())
                {
                    return process_error::resource_limit;
                }
                const auto success = release ? this->emulator_.memory.release_memory(address, static_cast<size_t>(size))
                                             : this->emulator_.memory.decommit_memory(address, static_cast<size_t>(size));
                return success ? process_error::none : process_error::unavailable;
            });
        }

        process_memory_result protect_memory(const uint64_t address, const uint64_t size, const memory_permission permission) override
        {
            return this->emulator_.synchronize([&] {
                if (size > std::numeric_limits<size_t>::max())
                {
                    return process_memory_result{.error = process_error::resource_limit};
                }
                nt_memory_permission old_permission{};
                if (!this->emulator_.memory.protect_memory(address, static_cast<size_t>(size), nt_memory_permission{permission},
                                                           &old_permission))
                {
                    return process_memory_result{.error = process_error::unavailable};
                }
                return process_memory_result{.address = address, .size = size, .permission = old_permission.common};
            });
        }

        process_memory_read_result read_memory(const uint64_t address, const uint64_t size) const override
        {
            return this->emulator_.synchronize([&] {
                if (size > std::numeric_limits<size_t>::max())
                {
                    return process_memory_read_result{.error = process_error::resource_limit};
                }
                process_memory_read_result result{};
                result.data.resize(static_cast<size_t>(size));
                if (!this->emulator_.memory.try_read_memory(address, result.data.data(), result.data.size()))
                {
                    return process_memory_read_result{.error = process_error::unavailable};
                }
                return result;
            });
        }

        process_memory_result write_memory(const uint64_t address, const std::span<const uint8_t> data) override
        {
            return this->emulator_.synchronize([&] {
                if (!this->emulator_.memory.try_write_memory(address, data.data(), data.size()))
                {
                    return process_memory_result{.error = process_error::unavailable};
                }
                return process_memory_result{.address = address, .size = data.size()};
            });
        }

      private:
        windows_emulator& emulator_;
    };
}

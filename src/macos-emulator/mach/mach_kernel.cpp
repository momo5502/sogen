#include "../std_include.hpp"
#include "mach_kernel.hpp"

#include <limits>
#include <numeric>
#include <stdexcept>
#include <utility>

namespace sogen
{
    namespace
    {
        bool is_task_special_port(const int32_t which)
        {
            return which > 0 && which < mach::task_special_port::max;
        }

        bool is_host_special_port(const int32_t which)
        {
            return which > 0 && which < mach::host_special_port::max;
        }

        mach::port_name_t lookup_special_port(const std::map<int32_t, mach::port_name_t>& ports, const int32_t which)
        {
            const auto entry = ports.find(which);
            return entry == ports.end() ? mach::PORT_NULL : entry->second;
        }
    }

    void mach_kernel::setup(const uint64_t main_thread_id)
    {
        this->thread_ports_[main_thread_id] =
            this->ports.allocate_receive_right({.kind = mach::kernel_object_kind::thread, .id = main_thread_id});
        this->task_self = this->ports.allocate_receive_right({.kind = mach::kernel_object_kind::task, .id = 1});
        this->host_self = this->ports.allocate_receive_right({.kind = mach::kernel_object_kind::host, .id = 1});
        this->bootstrap = this->ports.allocate_receive_right({.kind = mach::kernel_object_kind::bootstrap, .id = 1});

        this->task_special_ports_[mach::task_special_port::kernel] = this->task_self;
        this->task_special_ports_[mach::task_special_port::host] = this->host_self;
        this->task_special_ports_[mach::task_special_port::bootstrap] = this->bootstrap;
        this->host_special_ports_[mach::host_special_port::priv] = this->host_self;
    }

    mach::port_name_t mach_kernel::thread_self_for(const uint64_t thread_id)
    {
        const auto existing = this->thread_ports_.find(thread_id);
        if (existing != this->thread_ports_.end() && this->ports.exists(existing->second))
        {
            return existing->second;
        }

        const auto name = this->ports.allocate_receive_right({.kind = mach::kernel_object_kind::thread, .id = thread_id});
        this->thread_ports_[thread_id] = name;
        return name;
    }

    // libsystem_kernel keeps one special reply port per thread and re-uses it for every MIG call, which
    // is why a trace shows a single reply-port name for the whole run; a fresh name per call would be
    // visible to a guest that caches it.
    mach::port_name_t mach_kernel::make_special_reply_port(const uint64_t thread_id)
    {
        const auto existing = this->special_reply_ports_.find(thread_id);
        if (existing != this->special_reply_ports_.end() && this->ports.exists(existing->second))
        {
            return existing->second;
        }

        const auto name = this->ports.allocate_receive_right();
        this->special_reply_ports_[thread_id] = name;
        return name;
    }

    mach::port_name_t mach_kernel::get_task_special_port(const int32_t which) const
    {
        return lookup_special_port(this->task_special_ports_, which);
    }

    mach::kern_return_t mach_kernel::set_task_special_port(const int32_t which, const mach::port_name_t name)
    {
        if (!is_task_special_port(which))
        {
            return mach::kr::invalid_argument;
        }

        if (name != mach::PORT_NULL && !this->ports.exists(name))
        {
            return mach::kr::invalid_right;
        }

        this->task_special_ports_[which] = name;
        return mach::kr::success;
    }

    mach::port_name_t mach_kernel::get_host_special_port(const int32_t which) const
    {
        return lookup_special_port(this->host_special_ports_, which);
    }

    void mach_semaphore::serialize(utils::buffer_serializer& buffer) const
    {
        buffer.write(this->value);
        buffer.write(this->policy);
        buffer.write(this->id);
    }

    void mach_semaphore::deserialize(utils::buffer_deserializer& buffer)
    {
        buffer.read(this->value);
        buffer.read(this->policy);
        buffer.read(this->id);
    }

    void mach_kernel::adopt_counter_frequency(const uint64_t frequency_hz)
    {
        constexpr uint64_t nanoseconds_per_second = 1000000000;

        if (frequency_hz == 0 || frequency_hz > nanoseconds_per_second)
        {
            return;
        }

        const auto divisor = std::gcd(nanoseconds_per_second, frequency_hz);
        this->timebase_numer = static_cast<uint32_t>(nanoseconds_per_second / divisor);
        this->timebase_denom = static_cast<uint32_t>(frequency_hz / divisor);
    }

    mach::port_name_t mach_kernel::create_semaphore(const int32_t policy, const int32_t value)
    {
        const auto id = this->next_object_id_++;
        const auto name = this->ports.allocate_receive_right({.kind = mach::kernel_object_kind::semaphore, .id = id});
        if (name == mach::PORT_NULL)
        {
            return mach::PORT_NULL;
        }

        this->semaphores_[name] = mach_semaphore{.value = value, .policy = policy, .id = id};
        return name;
    }

    mach_semaphore* mach_kernel::find_semaphore(const mach::port_name_t name)
    {
        const auto entry = this->semaphores_.find(name);
        return entry == this->semaphores_.end() ? nullptr : &entry->second;
    }

    mach::kern_return_t mach_kernel::semaphore_signal(const mach::port_name_t name)
    {
        auto* semaphore = this->find_semaphore(name);
        if (semaphore == nullptr)
        {
            return mach::kr::invalid_name;
        }

        if (semaphore->value == std::numeric_limits<int32_t>::max())
        {
            return mach::kr::invalid_argument;
        }

        ++semaphore->value;
        return mach::kr::success;
    }

    // Stage 4 is single-threaded, so a zero count can never be signalled by anyone else. Timing out is
    // the honest answer; blocking would hang the emulator. Real waiting arrives with threading.
    mach::kern_return_t mach_kernel::semaphore_wait(const mach::port_name_t name)
    {
        auto* semaphore = this->find_semaphore(name);
        if (semaphore == nullptr)
        {
            return mach::kr::invalid_name;
        }

        if (semaphore->value <= 0)
        {
            return mach::kr::operation_timed_out;
        }

        --semaphore->value;
        return mach::kr::success;
    }

    mach::port_name_t mach_kernel::create_voucher()
    {
        const auto id = this->next_object_id_++;
        return this->ports.allocate_receive_right({.kind = mach::kernel_object_kind::voucher, .id = id});
    }

    void mach_memory_entry::serialize(utils::buffer_serializer& buffer) const
    {
        buffer.write(this->address);
        buffer.write(this->size);
    }

    void mach_memory_entry::deserialize(utils::buffer_deserializer& buffer)
    {
        buffer.read(this->address);
        buffer.read(this->size);
    }

    mach::port_name_t mach_kernel::create_memory_entry(const uint64_t address, const uint64_t size)
    {
        const auto id = this->next_object_id_++;
        const auto name = this->ports.allocate_receive_right({.kind = mach::kernel_object_kind::memory_entry, .id = id});
        if (name != mach::PORT_NULL)
        {
            this->memory_entries_[name] = mach_memory_entry{.address = address, .size = size};
        }

        return name;
    }

    const mach_memory_entry* mach_kernel::find_memory_entry(const mach::port_name_t name) const
    {
        const auto found = this->memory_entries_.find(name);
        return found == this->memory_entries_.end() ? nullptr : &found->second;
    }

    mach::port_name_t mach_kernel::io_master_port()
    {
        if (this->io_master_ == mach::PORT_NULL)
        {
            this->io_master_ = this->ports.allocate_receive_right({.kind = mach::kernel_object_kind::io_master, .id = 1});
        }

        return this->io_master_;
    }

    mach::port_name_t mach_kernel::create_timer()
    {
        const auto id = this->next_object_id_++;
        return this->ports.allocate_receive_right({.kind = mach::kernel_object_kind::timer, .id = id});
    }

    mach::kern_return_t mach_kernel::destroy_timer(const mach::port_name_t name)
    {
        if (this->ports.object_of(name).kind != mach::kernel_object_kind::timer)
        {
            return mach::kr::invalid_argument;
        }

        this->armed_timers_.erase(name);
        return this->ports.mod_refs(name, mach::right_kind::receive, -1);
    }

    bool mach_kernel::arm_timer(const mach::port_name_t name, const uint64_t deadline)
    {
        if (this->ports.object_of(name).kind != mach::kernel_object_kind::timer)
        {
            return false;
        }

        const auto was_armed = this->armed_timers_.contains(name);
        this->armed_timers_[name] = deadline;
        return was_armed;
    }

    uint64_t mach_kernel::cancel_timer(const mach::port_name_t name)
    {
        const auto armed = this->armed_timers_.find(name);
        if (armed == this->armed_timers_.end())
        {
            return 0;
        }

        const auto deadline = armed->second;
        this->armed_timers_.erase(armed);
        return deadline;
    }

    void mach_kernel::disarm_timer(const mach::port_name_t name)
    {
        this->armed_timers_.erase(name);
    }

    std::optional<mach_kernel::armed_timer> mach_kernel::earliest_armed_timer() const
    {
        std::optional<armed_timer> earliest{};

        for (const auto& [name, deadline] : this->armed_timers_)
        {
            if (!earliest.has_value() || deadline < earliest->deadline)
            {
                earliest = armed_timer{.name = name, .deadline = deadline};
            }
        }

        return earliest;
    }

    mach::port_name_t mach_kernel::clock_service(const uint32_t clock_id)
    {
        const auto existing = this->clock_ports_.find(clock_id);
        if (existing != this->clock_ports_.end())
        {
            return existing->second;
        }

        const auto name = this->ports.allocate_receive_right({.kind = mach::kernel_object_kind::clock, .id = clock_id});
        if (name != mach::PORT_NULL)
        {
            this->clock_ports_[clock_id] = name;
        }

        return name;
    }

    void mach_kernel::serialize(utils::buffer_serializer& buffer) const
    {
        this->ports.serialize(buffer);

        buffer.write(this->task_self);
        buffer.write(this->host_self);
        buffer.write(this->bootstrap);

        buffer.write_map(this->thread_ports_);
        buffer.write_map(this->special_reply_ports_);
        buffer.write_map(this->task_special_ports_);
        buffer.write_map(this->host_special_ports_);
        buffer.write_map(this->semaphores_);
        buffer.write_map(this->clock_ports_);
        buffer.write_map(this->armed_timers_);
        buffer.write_map(this->memory_entries_);
        buffer.write(this->io_master_);
        buffer.write(this->timebase_numer);
        buffer.write(this->timebase_denom);
        buffer.write(this->next_object_id_);
        buffer.write(this->all_image_info_address);
        buffer.write(this->all_image_info_size);
        this->exceptions.serialize(buffer);
        buffer.write_optional(this->last_exception);
        buffer.write(this->notifications);
    }

    void mach_kernel::deserialize(utils::buffer_deserializer& buffer)
    {
        mach::mach_port_namespace new_ports{};
        new_ports.deserialize(buffer);

        const auto new_task_self = buffer.read<mach::port_name_t>();
        const auto new_host_self = buffer.read<mach::port_name_t>();
        const auto new_bootstrap = buffer.read<mach::port_name_t>();

        auto new_thread_ports = buffer.read_map<std::map<uint64_t, mach::port_name_t>>();
        auto new_special_reply_ports = buffer.read_map<std::map<uint64_t, mach::port_name_t>>();
        auto new_task_special_ports = buffer.read_map<std::map<int32_t, mach::port_name_t>>();
        auto new_host_special_ports = buffer.read_map<std::map<int32_t, mach::port_name_t>>();
        auto new_semaphores = buffer.read_map<std::map<mach::port_name_t, mach_semaphore>>();
        auto new_clock_ports = buffer.read_map<std::map<uint32_t, mach::port_name_t>>();
        auto new_armed_timers = buffer.read_map<std::map<mach::port_name_t, uint64_t>>();
        auto new_memory_entries = buffer.read_map<std::map<mach::port_name_t, mach_memory_entry>>();
        const auto new_io_master = buffer.read<mach::port_name_t>();
        const auto new_timebase_numer = buffer.read<uint32_t>();
        const auto new_timebase_denom = buffer.read<uint32_t>();
        const auto new_next_object_id = buffer.read<uint64_t>();
        const auto new_all_image_info_address = buffer.read<uint64_t>();
        const auto new_all_image_info_size = buffer.read<uint64_t>();

        mach::exception_state new_exceptions{};
        new_exceptions.deserialize(buffer);
        std::optional<mach::raised_exception> new_last_exception{};
        buffer.read_optional(new_last_exception);
        auto new_notifications = buffer.read<std::vector<mach_port_notification>>();

        if (new_timebase_denom == 0)
        {
            throw std::runtime_error("Mach snapshot declares a zero timebase denominator");
        }

        for (const auto& entry : new_task_special_ports)
        {
            if (!is_task_special_port(entry.first))
            {
                throw std::runtime_error("Mach snapshot declares an out of range task special port " + std::to_string(entry.first));
            }
        }

        for (const auto& entry : new_host_special_ports)
        {
            if (!is_host_special_port(entry.first))
            {
                throw std::runtime_error("Mach snapshot declares an out of range host special port " + std::to_string(entry.first));
            }
        }

        this->ports = std::move(new_ports);
        this->task_self = new_task_self;
        this->host_self = new_host_self;
        this->bootstrap = new_bootstrap;
        this->thread_ports_ = std::move(new_thread_ports);
        this->special_reply_ports_ = std::move(new_special_reply_ports);
        this->task_special_ports_ = std::move(new_task_special_ports);
        this->host_special_ports_ = std::move(new_host_special_ports);
        this->semaphores_ = std::move(new_semaphores);
        this->clock_ports_ = std::move(new_clock_ports);
        this->armed_timers_ = std::move(new_armed_timers);
        this->memory_entries_ = std::move(new_memory_entries);
        this->io_master_ = new_io_master;
        this->timebase_numer = new_timebase_numer;
        this->timebase_denom = new_timebase_denom;
        this->next_object_id_ = new_next_object_id;
        this->all_image_info_address = new_all_image_info_address;
        this->all_image_info_size = new_all_image_info_size;
        this->exceptions = std::move(new_exceptions);
        this->last_exception = new_last_exception;
        this->notifications = std::move(new_notifications);
    }
}

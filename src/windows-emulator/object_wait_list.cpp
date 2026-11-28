#include "std_include.hpp"
#include "object_wait_list.hpp"
#include "emulator_thread.hpp"
#include "process_context.hpp"
#include "windows_objects.hpp"

namespace sogen
{
    namespace
    {
        void point_thread_link(const object_wait_list::entry& e, object_wait_list* list)
        {
            if (e.thread && e.index < e.thread->await_object_lists.size())
            {
                e.thread->await_object_lists[e.index] = list;
            }
        }
    }

    object_wait_list::~object_wait_list()
    {
        this->clear();
    }

    object_wait_list::object_wait_list(object_wait_list&& obj) noexcept
        : entries_(std::move(obj.entries_))
    {
        obj.entries_.clear();
        for (const auto& e : this->entries_)
        {
            point_thread_link(e, this);
        }
    }

    object_wait_list& object_wait_list::operator=(object_wait_list&& obj) noexcept
    {
        if (this != &obj)
        {
            this->clear();
            this->entries_ = std::move(obj.entries_);
            obj.entries_.clear();
            for (const auto& e : this->entries_)
            {
                point_thread_link(e, this);
            }
        }

        return *this;
    }

    void object_wait_list::clear()
    {
        for (const auto& e : this->entries_)
        {
            point_thread_link(e, nullptr);
        }

        this->entries_.clear();
    }

    void object_wait_list::link(emulator_thread& thread, const uint32_t index)
    {
        if (thread.await_object_lists.size() <= index)
        {
            thread.await_object_lists.resize(index + 1);
        }

        this->entries_.push_back({.thread = &thread, .index = index, .thread_id = thread.id});
        thread.await_object_lists[index] = this;
    }

    void object_wait_list::unlink(emulator_thread& thread, const uint32_t index)
    {
        const auto found = std::ranges::find_if(this->entries_, [&](const entry& e) {
            return e.thread == &thread && e.index == index; //
        });

        if (found != this->entries_.end())
        {
            this->entries_.erase(found);
        }

        if (index < thread.await_object_lists.size() && thread.await_object_lists[index] == this)
        {
            thread.await_object_lists[index] = nullptr;
        }
    }

    object_wait_list::entry object_wait_list::take_front()
    {
        const auto first = this->entries_.front();
        this->entries_.erase(this->entries_.begin());
        if (first.thread && first.index < first.thread->await_object_lists.size() && first.thread->await_object_lists[first.index] == this)
        {
            first.thread->await_object_lists[first.index] = nullptr;
        }

        return first;
    }

    void object_wait_list::serialize(utils::buffer_serializer& buffer) const
    {
        buffer.write(static_cast<uint64_t>(this->entries_.size()));
        for (const auto& e : this->entries_)
        {
            buffer.write(e.thread_id);
            buffer.write(e.index);
        }
    }

    void object_wait_list::deserialize(utils::buffer_deserializer& buffer)
    {
        this->clear();

        const auto count = buffer.read<uint64_t>();
        for (uint64_t i = 0; i < count; ++i)
        {
            entry e{};
            buffer.read(e.thread_id);
            buffer.read(e.index);
            this->entries_.push_back(e);
        }
    }

    void object_wait_list::restore_links(process_context& process)
    {
        std::vector<entry> restored{};
        restored.reserve(this->entries_.size());

        for (auto e : this->entries_)
        {
            auto* thread = process.find_thread_by_id(e.thread_id);
            if (!thread || e.index >= thread->await_objects.size())
            {
                continue;
            }

            if (thread->await_object_lists.size() < thread->await_objects.size())
            {
                thread->await_object_lists.resize(thread->await_objects.size());
            }

            e.thread = thread;
            thread->await_object_lists[e.index] = this;
            restored.push_back(e);
        }

        this->entries_ = std::move(restored);
    }

    event_signal signal_event(event& e)
    {
        if (e.set_signaled(true))
        {
            return {.was_signaled = true};
        }

        while (e.is_signaled() && !e.waiters.empty())
        {
            const auto first = e.waiters.take_front();
            if (!first.thread)
            {
                continue;
            }

            if (!e.try_acquire())
            {
                break;
            }

            first.thread->satisfy_object_wait(first.index, false);
        }

        return {.was_signaled = false};
    }

    void release_semaphore_units(semaphore& s, const uint32_t previous_count)
    {
        if (previous_count != 0)
        {
            return;
        }

        while (s.current_count > 0 && !s.waiters.empty())
        {
            const auto first = s.waiters.take_front();
            if (!first.thread)
            {
                continue;
            }

            --s.current_count;
            first.thread->satisfy_object_wait(first.index, false);
        }
    }

    void hand_over_mutant(mutant& m, const uint32_t process_id)
    {
        while (!m.is_locked() && !m.waiters.empty())
        {
            const auto first = m.waiters.take_front();
            if (!first.thread)
            {
                continue;
            }

            const auto abandoned = m.try_lock(mutant::make_owner(process_id, first.thread->id)).value_or(false);
            first.thread->satisfy_object_wait(first.index, abandoned);
        }
    }
}

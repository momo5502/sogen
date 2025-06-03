#pragma once
#include "logger.hpp"

struct mapped_module;
struct windows_emulator;

enum class event_type
{
    syscall,
    function_call,
};

struct event : utils::object
{
    event() = default;

    event(event&&) = delete;
    event& operator=(event&&) = delete;
    event(const event&) = delete;
    event& operator=(const event&) = delete;

    virtual event_type get_type() const = 0;

    virtual void print(const generic_logger& log) const
    {
        (void)log;
    }
};

template <event_type Type>
struct typed_event : event
{
    using event::event;

    event_type get_type() const override
    {
        return Type;
    }
};

template <event_type Type, typename Data>
class rich_event : typed_event<Type>
{
  public:
    rich_event(windows_emulator& win_emu, Data data)
        : win_emu(&win_emu),
          data(std::move(data))
    {
    }

    const Data& get_data() const
    {
        return this->data;
    }

  protected:
    windows_emulator* win_emu{};
    Data data{};
};

struct syscall_data
{
    uint32_t id{};
    std::string_view name{};
};

struct syscall_event : rich_event<event_type::syscall, syscall_data>
{
    struct extended_info
    {
        uint64_t address{};
        mapped_module* origin{};
    };

    extended_info get_extended_info() const;
};

struct event_manager : utils::object
{
    virtual void handle(const event& e);
};

class printing_event_manager : public event_manager
{
  public:
    printing_event_manager(generic_logger& log)
        : logger_(&log)
    {
    }

    void handle(const event& e) override
    {
        e.print(*this->logger_);
    }

  private:
    generic_logger* logger_{};
};

#pragma once
#include "logger.hpp"

struct mapped_module;
class windows_emulator;

enum class emulation_event_type
{
    syscall,
    function_call,
};

struct emulation_event : utils::object
{
    emulation_event() = default;

    emulation_event(emulation_event&&) = delete;
    emulation_event& operator=(emulation_event&&) = delete;
    emulation_event(const emulation_event&) = delete;
    emulation_event& operator=(const emulation_event&) = delete;

    virtual emulation_event_type get_type() const = 0;

    virtual void print(generic_logger& log) const
    {
        (void)log;
    }
};

template <emulation_event_type Type>
struct typed_event : emulation_event
{
    using emulation_event::emulation_event;

    emulation_event_type get_type() const override
    {
        return Type;
    }
};

struct empty_data
{
};

template <emulation_event_type Type, typename Input = empty_data, typename Output = empty_data>
class rich_event : public typed_event<Type>
{
  public:
    rich_event(windows_emulator& win_emu, Input input = {}, Output output = {})
        : win_emu(&win_emu),
          in(std::move(input))
    {
    }

    const Input& get_input() const
    {
        return this->in;
    }

    Output& get_output()
    {
        return this->out;
    }

    const Output& get_output() const
    {
        return this->out;
    }

  protected:
    windows_emulator* win_emu{};
    Input in{};
    Output out{};
};

struct syscall_input
{
    uint32_t id{};
    std::string_view name{};
};

struct syscall_output
{
    bool skip{false};
};

struct syscall_event : rich_event<emulation_event_type::syscall, syscall_input, syscall_output>
{
    using rich_event::rich_event;

    void print(generic_logger& log) const override;
};

struct event_manager : utils::object
{
    virtual void handle(emulation_event& e);
};

class printing_event_manager : public event_manager
{
  public:
    printing_event_manager(generic_logger& log)
        : logger_(&log)
    {
    }

    void handle(emulation_event& e) override
    {
        e.print(*this->logger_);
    }

  private:
    generic_logger* logger_{};
};

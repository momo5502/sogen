#include "std_include.hpp"
#include "tenet_tracer.hpp"
#include <utils/finally.hpp>

#include <iomanip>

namespace
{
    std::string format_hex(uint64_t value)
    {
        std::stringstream ss;
        ss << "0x" << std::hex << value;
        return ss.str();
    }

    std::string format_byte_array(const uint8_t* data, size_t size)
    {
        std::stringstream ss;
        for (size_t i = 0; i < size; ++i)
        {
            ss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(data[i]);
        }
        return ss.str();
    }

    void parse_and_accumulate_changes(const std::string& line, std::map<std::string, std::string>& changes)
    {
        size_t start = 0;
        while (start < line.length())
        {
            size_t end = line.find(',', start);
            if (end == std::string::npos)
            {
                end = line.length();
            }

            std::string pair_str = line.substr(start, end - start);
            size_t equals_pos = pair_str.find('=');
            if (equals_pos != std::string::npos)
            {
                std::string key = pair_str.substr(0, equals_pos);
                std::string value = pair_str.substr(equals_pos + 1);
                changes[key] = value;
            }

            start = end + 1;
        }
    }
}

tenet_tracer::tenet_tracer(windows_emulator& win_emu, const std::filesystem::path& log_filename)
    : win_emu_(win_emu),
      log_file_(log_filename)
{
    if (!log_file_)
    {
        throw std::runtime_error("TenetTracer: Failed to open log file -> " + log_filename.string());
    }

    auto& emu = win_emu_.emu();

    auto* read_hook = emu.hook_memory_read(0, 0xFFFFFFFFFFFFFFFF, [this](uint64_t a, const void* d, size_t s) {
        this->log_memory_read(a, d, s); //
    });
    read_hook_ = scoped_hook(emu, read_hook);

    auto* write_hook = emu.hook_memory_write(0, 0xFFFFFFFFFFFFFFFF, [this](uint64_t a, const void* d, size_t s) {
        this->log_memory_write(a, d, s); //
    });
    write_hook_ = scoped_hook(emu, write_hook);

    auto* execute_hook = emu.hook_memory_execution([&](uint64_t address) {
        this->process_instruction(address); //
    });
    execute_hook_ = scoped_hook(emu, execute_hook);
}

tenet_tracer::~tenet_tracer()
{
    filter_and_write_buffer();

    if (log_file_.is_open())
    {
        log_file_.close();
    }
}

void tenet_tracer::filter_and_write_buffer()
{
    if (raw_log_buffer_.empty())
    {
        return;
    }

    const auto* exe_module = win_emu_.mod_manager.executable;
    if (!exe_module)
    {
        for (const auto& line : raw_log_buffer_)
        {
            log_file_ << line << '\n';
        }

        return;
    }

    if (!raw_log_buffer_.empty())
    {
        log_file_ << raw_log_buffer_.front() << '\n';
    }

    bool currently_outside = false;
    std::map<std::string, std::string> accumulated_changes;

    for (size_t i = 1; i < raw_log_buffer_.size(); ++i)
    {
        const auto& line = raw_log_buffer_[i];

        size_t rip_pos = line.find("rip=0x");
        if (rip_pos == std::string::npos)
        {
            continue;
        }

        char* end_ptr = nullptr;
        uint64_t address = std::strtoull(line.c_str() + rip_pos + 6, &end_ptr, 16);

        bool is_line_inside = exe_module->is_within(address);
        const auto _1 = utils::finally([&] {
            currently_outside = !is_line_inside; //
        });

        if (!is_line_inside)
        {
            parse_and_accumulate_changes(line, accumulated_changes);
            continue;
        }

        const auto _2 = utils::finally([&] {
            log_file_ << line << '\n'; //
        });

        if (!currently_outside || accumulated_changes.empty())
        {
            continue;
        }

        std::stringstream summary_line;
        bool first = true;

        auto rip_it = accumulated_changes.find("rip");
        std::string last_rip;
        if (rip_it != accumulated_changes.end())
        {
            last_rip = rip_it->second;
            accumulated_changes.erase(rip_it);
        }

        for (const auto& pair : accumulated_changes)
        {
            if (!first)
            {
                summary_line << ",";
            }
            summary_line << pair.first << "=" << pair.second;
            first = false;
        }

        if (!last_rip.empty())
        {
            if (!first)
            {
                summary_line << ",";
            }
            summary_line << "rip=" << last_rip;
        }

        log_file_ << summary_line.str() << '\n';
        accumulated_changes.clear();
    }

    raw_log_buffer_.clear();
}

void tenet_tracer::log_memory_read(uint64_t address, const void* data, size_t size)
{
    if (!mem_read_log_.str().empty())
    {
        mem_read_log_ << ";";
    }

    mem_read_log_ << format_hex(address) << ":" << format_byte_array(static_cast<const uint8_t*>(data), size);
}

void tenet_tracer::log_memory_write(uint64_t address, const void* data, size_t size)
{
    if (!mem_write_log_.str().empty())
    {
        mem_write_log_ << ";";
    }

    mem_write_log_ << format_hex(address) << ":" << format_byte_array(static_cast<const uint8_t*>(data), size);
}

void tenet_tracer::process_instruction(const uint64_t address)
{
    auto& emu = win_emu_.emu();
    std::stringstream trace_line;

    std::array<uint64_t, GPRs_TO_TRACE.size()> current_regs{};
    for (size_t i = 0; i < GPRs_TO_TRACE.size(); ++i)
    {
        current_regs[i] = emu.reg<uint64_t>(GPRs_TO_TRACE[i].first);
    }

    bool first_entry = true;
    auto append_separator = [&] {
        if (!first_entry)
        {
            trace_line << ",";
        }
        first_entry = false;
    };

    if (is_first_instruction_)
    {
        for (size_t i = 0; i < GPRs_TO_TRACE.size(); ++i)
        {
            append_separator();
            trace_line << GPRs_TO_TRACE[i].second << "=" << format_hex(current_regs[i]);
        }
        is_first_instruction_ = false;
    }
    else
    {
        for (size_t i = 0; i < GPRs_TO_TRACE.size(); ++i)
        {
            if (previous_registers_[i] != current_regs[i])
            {
                append_separator();
                trace_line << GPRs_TO_TRACE[i].second << "=" << format_hex(current_regs[i]);
            }
        }
    }

    append_separator();
    trace_line << "rip=" << format_hex(address);

    const auto mem_reads = mem_read_log_.str();
    if (!mem_reads.empty())
    {
        append_separator();
        trace_line << "mr=" << mem_reads;
    }

    const auto mem_writes = mem_write_log_.str();
    if (!mem_writes.empty())
    {
        append_separator();
        trace_line << "mw=" << mem_writes;
    }

    raw_log_buffer_.push_back(trace_line.str());
    previous_registers_ = current_regs;

    mem_read_log_.str("");
    mem_read_log_.clear();
    mem_write_log_.str("");
    mem_write_log_.clear();
}

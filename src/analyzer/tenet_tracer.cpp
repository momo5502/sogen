#include "tenet_tracer.hpp"
#include <iomanip>
#include <map>

TenetTracer::TenetTracer(windows_emulator& win_emu, const std::string& log_filename)
    : m_win_emu(win_emu),
      m_log_file(log_filename)
{
    if (!m_log_file.is_open())
    {
        throw std::runtime_error("TenetTracer: Failed to open log file -> " + log_filename);
    }
    // Set up memory hooks.
    auto& emu = m_win_emu.emu();
    m_read_hook = emu.hook_memory_read(0, 0xFFFFFFFFFFFFFFFF,
                                       [this](uint64_t a, const void* d, size_t s) { this->log_memory_read(a, d, s); });
    m_write_hook = emu.hook_memory_write(
        0, 0xFFFFFFFFFFFFFFFF, [this](uint64_t a, const void* d, size_t s) { this->log_memory_write(a, d, s); });
}

TenetTracer::~TenetTracer()
{
    auto& emu = m_win_emu.emu();
    if (m_read_hook)
        emu.delete_hook(m_read_hook);
    if (m_write_hook)
        emu.delete_hook(m_write_hook);

    // Filter and write the buffer when the program ends.
    filter_and_write_buffer();

    if (m_log_file.is_open())
    {
        m_log_file.close();
    }
}

// Helper function: Parses a log line and adds register changes to the map.
static void parse_and_accumulate_changes(const std::string& line, std::map<std::string, std::string>& changes)
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
            changes[key] = value; // Updates existing or adds a new one.
        }

        start = end + 1;
    }
}

void TenetTracer::filter_and_write_buffer()
{
    if (m_raw_log_buffer.empty())
    {
        return;
    }

    const auto* exe_module = m_win_emu.mod_manager.executable;
    if (!exe_module)
    {
        // If there is no main module, write the raw data and exit.
        for (const auto& line : m_raw_log_buffer)
        {
            m_log_file << line << '\n';
        }
        return;
    }

    // Always write the first line (initial registers).
    if (!m_raw_log_buffer.empty())
    {
        m_log_file << m_raw_log_buffer.front() << '\n';
    }

    bool currently_outside = false;
    std::map<std::string, std::string> accumulated_changes;

    for (size_t i = 1; i < m_raw_log_buffer.size(); ++i)
    {
        const auto& line = m_raw_log_buffer[i];

        size_t rip_pos = line.find("rip=0x");
        if (rip_pos == std::string::npos)
            continue;

        char* end_ptr;
        uint64_t address = std::strtoull(line.c_str() + rip_pos + 6, &end_ptr, 16);

        bool is_line_inside = exe_module->is_within(address);

        if (is_line_inside)
        {
            // We are inside the main module.
            if (currently_outside)
            {
                // JUST ENTERED FROM OUTSIDE (moment of return from API)
                // 1. Create a synthetic log line from the accumulated changes.
                if (!accumulated_changes.empty())
                {
                    std::stringstream summary_line;
                    bool first = true;

                    // Separate rip from the map as it will be added at the end.
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
                            summary_line << ",";
                        summary_line << pair.first << "=" << pair.second;
                        first = false;
                    }

                    // Add the last known rip at the end.
                    if (!last_rip.empty())
                    {
                        if (!first)
                            summary_line << ",";
                        summary_line << "rip=" << last_rip;
                    }

                    m_log_file << summary_line.str() << '\n';
                }
                accumulated_changes.clear();
            }

            // 2. Write the current line within the main module.
            m_log_file << line << '\n';
            currently_outside = false;
        }
        else
        {
            // We are outside the main module.
            // 1. Accumulate the changes.
            parse_and_accumulate_changes(line, accumulated_changes);
            currently_outside = true;
        }
    }

    m_raw_log_buffer.clear();
}

std::string TenetTracer::format_hex(uint64_t value)
{
    std::stringstream ss;
    ss << "0x" << std::hex << value;
    return ss.str();
}

std::string TenetTracer::format_byte_array(const uint8_t* data, size_t size)
{
    std::stringstream ss;
    for (size_t i = 0; i < size; ++i)
    {
        ss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(data[i]);
    }
    return ss.str();
}

void TenetTracer::log_memory_read(uint64_t address, const void* data, size_t size)
{
    if (!m_mem_read_log.str().empty())
    {
        m_mem_read_log << ";";
    }
    m_mem_read_log << format_hex(address) << ":" << format_byte_array(static_cast<const uint8_t*>(data), size);
}

void TenetTracer::log_memory_write(uint64_t address, const void* data, size_t size)
{
    if (!m_mem_write_log.str().empty())
    {
        m_mem_write_log << ";";
    }
    m_mem_write_log << format_hex(address) << ":" << format_byte_array(static_cast<const uint8_t*>(data), size);
}

void TenetTracer::process_instruction(uint64_t address)
{
    auto& emu = m_win_emu.emu();
    std::stringstream trace_line;

    std::array<uint64_t, GPRs_TO_TRACE.size()> current_regs;
    for (size_t i = 0; i < GPRs_TO_TRACE.size(); ++i)
    {
        current_regs[i] = emu.reg<uint64_t>(GPRs_TO_TRACE[i].first);
    }

    bool first_entry = true;
    auto append_separator = [&]() {
        if (!first_entry)
        {
            trace_line << ",";
        }
        first_entry = false;
    };

    if (m_is_first_instruction)
    {
        for (size_t i = 0; i < GPRs_TO_TRACE.size(); ++i)
        {
            append_separator();
            trace_line << GPRs_TO_TRACE[i].second << "=" << format_hex(current_regs[i]);
        }
        m_is_first_instruction = false;
    }
    else
    {
        for (size_t i = 0; i < GPRs_TO_TRACE.size(); ++i)
        {
            if (m_previous_regs[i] != current_regs[i])
            {
                append_separator();
                trace_line << GPRs_TO_TRACE[i].second << "=" << format_hex(current_regs[i]);
            }
        }
    }

    append_separator();
    trace_line << "rip=" << format_hex(address);

    std::string mem_reads = m_mem_read_log.str();
    if (!mem_reads.empty())
    {
        append_separator();
        trace_line << "mr=" << mem_reads;
    }
    std::string mem_writes = m_mem_write_log.str();
    if (!mem_writes.empty())
    {
        append_separator();
        trace_line << "mw=" << mem_writes;
    }

    // Add the data to the buffer instead of writing directly to the file.
    m_raw_log_buffer.push_back(trace_line.str());

    m_previous_regs = current_regs;

    m_mem_read_log.str("");
    m_mem_read_log.clear();
    m_mem_write_log.str("");
    m_mem_write_log.clear();
}
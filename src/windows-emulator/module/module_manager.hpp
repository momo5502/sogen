#pragma once
#include <emulator.hpp>

#include "mapped_module.hpp"
#include "../file_system.hpp"
#include <utils/function.hpp>
#include "platform/win_pefile.hpp"

class logger;

// Execution mode for the emulated process
enum class execution_mode
{
    native_64bit, // Native 64-bit execution
    wow64_32bit,  // WOW64 mode for 32-bit applications
    unknown       // Detection failed or unsupported
};

// PE architecture detection result
struct pe_detection_result
{
    winpe::pe_arch architecture;
    execution_mode suggested_mode;
    std::string error_message;

    bool is_valid() const
    {
        return error_message.empty();
    }
};

// Strategy interface for module mapping
class module_mapping_strategy
{
  public:
    virtual ~module_mapping_strategy() = default;
    virtual mapped_module map_from_file(memory_manager& memory, std::filesystem::path file) = 0;
    virtual mapped_module map_from_memory(memory_manager& memory, uint64_t base_address, uint64_t image_size,
                                          const std::string& module_name) = 0;
};

// PE32 mapping strategy implementation
class pe32_mapping_strategy : public module_mapping_strategy
{
  public:
    mapped_module map_from_file(memory_manager& memory, std::filesystem::path file) override;
    mapped_module map_from_memory(memory_manager& memory, uint64_t base_address, uint64_t image_size,
                                  const std::string& module_name) override;
};

// PE64 mapping strategy implementation
class pe64_mapping_strategy : public module_mapping_strategy
{
  public:
    mapped_module map_from_file(memory_manager& memory, std::filesystem::path file) override;
    mapped_module map_from_memory(memory_manager& memory, uint64_t base_address, uint64_t image_size,
                                  const std::string& module_name) override;
};

// Factory for creating mapping strategies
class mapping_strategy_factory
{
  private:
    std::unique_ptr<pe32_mapping_strategy> pe32_strategy_;
    std::unique_ptr<pe64_mapping_strategy> pe64_strategy_;

  public:
    mapping_strategy_factory();
    module_mapping_strategy& get_strategy(winpe::pe_arch arch);
};

// PE architecture detector utility class
class pe_architecture_detector
{
  public:
    static pe_detection_result detect_from_file(const std::filesystem::path& file);
    static pe_detection_result detect_from_memory(uint64_t base_address, uint64_t image_size);
    static execution_mode determine_execution_mode(winpe::pe_arch executable_arch);
};

class module_manager
{
  public:
    struct callbacks
    {
        utils::optional_function<void(mapped_module& mod)> on_module_load{};
        utils::optional_function<void(mapped_module& mod)> on_module_unload{};
    };

    using module_map = serialized_map<uint64_t, mapped_module>;

    module_manager(memory_manager& memory, file_system& file_sys, callbacks& cb);

    void map_main_modules(const windows_path& executable_path, const windows_path& system32_path, const windows_path& syswow64_path,
                          const logger& logger);

    mapped_module* map_module(const windows_path& file, const logger& logger, bool is_static = false);
    mapped_module* map_local_module(const std::filesystem::path& file, const logger& logger, bool is_static = false);
    mapped_module* map_memory_module(uint64_t base_address, uint64_t image_size, const std::string& module_name, const logger& logger,
                                     bool is_static = false);

    mapped_module* find_by_address(const uint64_t address)
    {
        const auto entry = this->get_module(address);
        if (entry != this->modules_.end())
        {
            return &entry->second;
        }

        return nullptr;
    }

    mapped_module* find_by_name(const std::string_view name)
    {
        for (auto& mod : this->modules_ | std::views::values)
        {
            if (mod.name == name)
            {
                return &mod;
            }
        }

        return nullptr;
    }

    const char* find_name(const uint64_t address)
    {
        const auto* mod = this->find_by_address(address);
        if (!mod)
        {
            return "<N/A>";
        }

        return mod->name.c_str();
    }

    void serialize(utils::buffer_serializer& buffer) const;
    void deserialize(utils::buffer_deserializer& buffer);

    bool unmap(uint64_t address);
    const module_map& modules() const
    {
        return modules_;
    }

    // Execution mode accessors
    execution_mode get_execution_mode() const
    {
        return current_execution_mode_;
    }
    bool is_wow64_process() const
    {
        return current_execution_mode_ == execution_mode::wow64_32bit;
    }

    // TODO: These should be properly encapsulated. A good mechanism for quick module access is needed.
    mapped_module* executable{};
    mapped_module* ntdll{};
    mapped_module* win32u{};

    // WOW64-specific modules (for validation and future use)
    struct wow64_modules
    {
        mapped_module* ntdll32 = nullptr;      // 32-bit ntdll.dll
        mapped_module* wow64_dll = nullptr;    // wow64.dll (loaded by system)
        mapped_module* wow64win_dll = nullptr; // wow64win.dll (loaded by system)
        // Note: wow64cpu.dll is loaded by ntdll via registry lookup, not managed here
    } wow64_modules_;

  private:
    memory_manager* memory_{};
    file_system* file_sys_{};
    callbacks* callbacks_{};

    module_map modules_{};
    mutable module_map::iterator last_module_cache_{modules_.end()};

    // Strategy pattern components
    mapping_strategy_factory strategy_factory_;
    execution_mode current_execution_mode_ = execution_mode::unknown;

    // Core mapping logic to eliminate code duplication
    mapped_module* map_module_core(const pe_detection_result& detection_result, const std::function<mapped_module()>& mapper,
                                   const logger& logger, bool is_static);

    // Execution mode detection
    execution_mode detect_execution_mode(const windows_path& executable_path, const logger& logger);

    // Module loading helpers
    void load_native_64bit_modules(const windows_path& executable_path, const windows_path& ntdll_path, const windows_path& win32u_path,
                                   const logger& logger);
    void load_wow64_modules(const windows_path& executable_path, const windows_path& ntdll_path, const windows_path& win32u_path,
                            const windows_path& ntdll32_path, const logger& logger);

    void install_wow64_heaven_gate(const logger& logger);

    module_map::iterator get_module(const uint64_t address)
    {
        if (last_module_cache_ != this->modules_.end() && last_module_cache_->second.contains(address))
        {
            return last_module_cache_;
        }

        if (this->modules_.empty())
        {
            return this->modules_.end();
        }

        auto upper_bound = this->modules_.upper_bound(address);
        if (upper_bound == this->modules_.begin())
        {
            return this->modules_.end();
        }

        std::advance(upper_bound, -1);

        if (upper_bound->second.contains(address))
        {
            last_module_cache_ = upper_bound;
            return upper_bound;
        }

        return this->modules_.end();
    }
};

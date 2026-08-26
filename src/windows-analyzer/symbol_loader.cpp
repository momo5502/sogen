#include "symbol_loader.hpp"

#include "map_symbol_source.hpp"
#include "pdb_symbol_source.hpp"

#include <windows_emulator.hpp>

namespace sogen
{
    namespace
    {
        bool should_load_symbols_for_module(const windows_emulator& win_emu, const mapped_module& mod, const symbol_options& options,
                                            const bool for_callers)
        {
            if (for_callers)
            {
                return options.use_for_callers();
            }

            return options.use_for_trace() &&
                   (options.verbose_logging || win_emu.mod_manager.executable == &mod || options.interesting_modules.contains(mod.name));
        }

        void merge_symbols(mapped_module& mod, const loaded_symbols& symbols)
        {
            for (const auto& [rva, name] : symbols.address_names)
            {
                mod.symbol_address_names.try_emplace(mod.image_base + rva, name);
            }
        }
    }

    symbol_loader::symbol_loader(windows_emulator& win_emu, symbol_options options)
        : win_emu_(&win_emu),
          options_(std::move(options))
    {
        this->options_.pdb.verbose_logging = this->options_.verbose_logging;
        if (this->options_.pdb.enabled())
        {
            this->pdb_ = std::make_unique<pdb_symbol_source>(win_emu, this->options_.pdb);
        }
        if (!this->options_.maps.empty())
        {
            this->map_ = std::make_unique<map_symbol_source>(win_emu, this->options_.maps);
        }

        this->module_load_callback_ =
            this->win_emu_->callbacks.on_module_load.add([this](mapped_module& mod) { this->load_for_module(mod, false); });
        this->module_unload_callback_ =
            this->win_emu_->callbacks.on_module_unload.add([this](mapped_module& mod) { this->attempted_modules_.erase(mod.image_base); });

        this->load_existing_modules();
    }

    symbol_loader::~symbol_loader()
    {
        if (this->win_emu_ && this->module_load_callback_)
        {
            this->win_emu_->callbacks.on_module_load.remove(this->module_load_callback_);
        }
        if (this->win_emu_ && this->module_unload_callback_)
        {
            this->win_emu_->callbacks.on_module_unload.remove(this->module_unload_callback_);
        }
    }

    void symbol_loader::load_existing_modules()
    {
        if (!this->options_.use_for_trace())
        {
            return;
        }

        for (auto& mod : this->win_emu_->mod_manager.modules() | std::views::values)
        {
            this->load_for_module(mod, false);
        }
    }

    void symbol_loader::ensure_caller_symbols(const uint64_t address)
    {
        if (!this->options_.use_for_callers())
        {
            return;
        }

        auto* mod = this->win_emu_->mod_manager.executable && this->win_emu_->mod_manager.executable->contains(address)
                        ? this->win_emu_->mod_manager.executable
                        : this->win_emu_->mod_manager.find_by_address(address);
        if (mod)
        {
            this->load_for_module(*mod, true);
        }
    }

    void symbol_loader::load_for_module(mapped_module& mod, const bool for_callers)
    {
        if (mod.pe_sections.empty())
        {
            if (this->options_.verbose_logging)
            {
                this->win_emu_->log.info("Skipping symbols for %s: no PE section metadata\n", mod.name.c_str());
            }
            return;
        }

        if (!should_load_symbols_for_module(*this->win_emu_, mod, this->options_, for_callers) ||
            !this->attempted_modules_.insert(mod.image_base).second)
        {
            return;
        }

        if (this->map_)
        {
            try
            {
                merge_symbols(mod, this->map_->load(mod));
            }
            catch (const std::exception& e)
            {
                this->win_emu_->log.warn("Failed to load MAP symbols for %s: %s\n", mod.name.c_str(), e.what());
            }
        }
        if (this->pdb_)
        {
            try
            {
                merge_symbols(mod, this->pdb_->load(mod));
            }
            catch (const std::exception& e)
            {
                this->win_emu_->log.warn("Failed to load PDB symbols for %s: %s\n", mod.name.c_str(), e.what());
            }
        }

        mod.has_symbols = !mod.symbol_address_names.empty();
    }

} // namespace sogen

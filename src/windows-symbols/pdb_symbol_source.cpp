#include "pdb_symbol_source.hpp"
#include "pdb_file.hpp"
#include "symbol_cache.hpp"
#include "symbol_server.hpp"

#include <windows_emulator.hpp>
#include <utils/finally.hpp>
#include <utils/io.hpp>
#include <utils/string.hpp>
#include <platform/win_pefile_debug.hpp>

#include <filesystem>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace sogen::symbols
{
    namespace
    {
        using namespace std::literals;

        struct resolved_pdb
        {
            std::filesystem::path path{};
            bool explicit_input{};
            pdb_origin source{};
            bool temporary{};
        };

        bool pdb_validation_failed(const pdb_validation_status status)
        {
            return status == pdb_validation_status::invalid_artifact || status == pdb_validation_status::validation_error;
        }

        pdb_origin resolution_source(const symbol_server::source source)
        {
            switch (source)
            {
            case symbol_server::source::cache:
                return pdb_origin::cache;
            case symbol_server::source::symbol_store:
                return pdb_origin::symbol_store;
            case symbol_server::source::download:
                return pdb_origin::download;
            }

            return pdb_origin::download;
        }

        std::optional<resolved_pdb> resolve_from_signature(const pdb_signature& sig, const pdb_symbol_options& options,
                                                           const symbol_event_callback& on_event, const std::string_view module_name,
                                                           const std::optional<std::filesystem::path>& module_pe_path = std::nullopt)
        {
            if (!sig.valid() || !sig.has_safe_filename())
            {
                return std::nullopt;
            }

            if (module_pe_path && !module_pe_path->empty())
            {
                std::vector<std::filesystem::path> sibling_candidates{};

                auto sibling_pdb = *module_pe_path;
                sibling_pdb.replace_extension(".pdb");
                sibling_candidates.push_back(std::move(sibling_pdb));

                if (!sig.filename().empty())
                {
                    sibling_candidates.push_back(module_pe_path->parent_path() / sig.filename());
                }

                for (const auto& candidate : sibling_candidates)
                {
                    if (!utils::io::file_exists(candidate))
                    {
                        continue;
                    }

                    const auto validation = pdb_file{candidate}.validate(sig);
                    if (validation.status == pdb_validation_status::mismatch)
                    {
                        continue;
                    }
                    if (pdb_validation_failed(validation.status))
                    {
                        on_event(pdb_lookup_event{
                            .status = pdb_lookup_result::candidate_validation_failed,
                            .module = std::string{module_name},
                            .path = candidate,
                            .error = validation.error,
                        });
                        continue;
                    }

                    return resolved_pdb{
                        .path = candidate,
                        .explicit_input = false,
                        .source = pdb_origin::sibling_path,
                    };
                }
            }

            if (const auto cached = symbol_cache::find(sig, options.symbol_cache, on_event, module_name))
            {
                return resolved_pdb{
                    .path = *cached,
                    .explicit_input = false,
                    .source = pdb_origin::cache,
                };
            }

            if (const auto resolved = symbol_server::find(sig, options, on_event, module_name))
            {
                return resolved_pdb{
                    .path = resolved->path,
                    .explicit_input = false,
                    .source = resolution_source(resolved->origin),
                    .temporary = resolved->temporary,
                };
            }

            return std::nullopt;
        }

        void merge_symbols(loaded_symbols& result, const mapped_module& mod, const pdb_file& pdb)
        {
            for (const auto& [address, name] : pdb.symbols)
            {
                if (const auto rva = symbol_address_to_rva(mod, address.first, address.second))
                {
                    result.address_names.try_emplace(*rva, name);
                }
            }
        }

        bool matches_module(const mapped_module& mod, const pdb_file& pdb)
        {
            return mod.pdb && pdb.matches(*mod.pdb);
        }

    }

    pdb_symbol_source::pdb_symbol_source(windows_emulator& win_emu, pdb_symbol_options options)
        : win_emu_(&win_emu),
          options_(std::move(options))
    {
    }

    loaded_symbols pdb_symbol_source::load(const mapped_module& mod) const
    {
        loaded_symbols result{};
        if (!this->options_.enabled())
        {
            return result;
        }

        std::call_once(this->cache_initialized_, [this] { symbol_cache::initialize(this->on_event, this->options_.symbol_cache); });

        std::vector<resolved_pdb> candidates{};
        auto auto_lookup = this->options_.auto_lookup;

        for (const auto& input : this->options_.inputs)
        {
            if (input.module ? !(utils::string::equals_ignore_case(*input.module, mod.name) ||
                                 (!mod.path.empty() && utils::string::equals_ignore_case(*input.module, mod.path.filename().string())))
                             : this->win_emu_->mod_manager.executable != &mod)
            {
                continue;
            }

            if (input.path.empty())
            {
                auto_lookup = true;
                continue;
            }

            candidates.emplace_back(resolved_pdb{
                .path = input.path,
                .explicit_input = true,
                .source = pdb_origin::explicit_path,
            });
        }

        if (auto_lookup && mod.pdb)
        {
            auto module_path = mod.path.u16string();
            if (module_path.size() > 2 && module_path[1] == u'/')
            {
                module_path.insert(1, 1, u':');
            }
            const auto module_pe_path = module_path.empty() ? std::nullopt : this->win_emu_->file_sys.translate(windows_path{module_path});

            if (!mod.pdb->has_safe_filename())
            {
                this->on_event(pdb_lookup_event{
                    .status = pdb_lookup_result::invalid_filename,
                    .module = mod.name,
                });
            }
            else if (const auto resolved = resolve_from_signature(*mod.pdb, this->options_, this->on_event, mod.name, module_pe_path))
            {
                candidates.emplace_back(*resolved);
                this->on_event(pdb_lookup_event{
                    .status = pdb_lookup_result::found,
                    .module = mod.name,
                });
            }
            else
            {
                this->on_event(pdb_lookup_event{
                    .status = pdb_lookup_result::not_found,
                    .module = mod.name,
                });
            }
        }
        else if (auto_lookup)
        {
            this->on_event(pdb_lookup_event{
                .status = pdb_lookup_result::no_reference,
                .module = mod.name,
            });
        }

        if (candidates.empty())
        {
            this->on_event(pdb_lookup_event{
                .status = pdb_lookup_result::no_candidates,
                .module = mod.name,
            });
        }

        std::set<std::filesystem::path> loaded_candidates{};
        for (const auto& candidate : candidates)
        {
            if (!loaded_candidates.insert(candidate.path).second)
            {
                continue;
            }

            const auto cleanup = utils::finally([&] {
                if (candidate.temporary)
                {
                    std::error_code ec{};
                    std::filesystem::remove_all(candidate.path.parent_path(), ec);
                }
            });

            this->on_event(pdb_load_event{
                .status = pdb_load_status::started,
                .module = mod.name,
                .path = candidate.path,
                .origin = candidate.source,
                .error = {},
            });

            pdb_file pdb{candidate.path};
            const auto read_result = pdb.read();
            if (!read_result.success)
            {
                if (candidate.explicit_input)
                {
                    throw std::runtime_error("Failed to parse PDB " + candidate.path.string() + ": " + read_result.error);
                }

                this->on_event(pdb_load_event{
                    .status = pdb_load_status::parse_failed,
                    .module = mod.name,
                    .path = candidate.path,
                    .origin = candidate.source,
                    .error = read_result.error,
                });
                continue;
            }

            if (!matches_module(mod, pdb))
            {
                const auto candidate_filename = candidate.path.filename().string();
                const auto same_pdb_name =
                    mod.pdb && utils::string::equals_ignore_case(mod.pdb->filename(), std::string_view{candidate_filename});
                if (candidate.explicit_input && same_pdb_name)
                {
                    throw std::runtime_error("PDB signature mismatch for " + candidate.path.string() + " and module " + mod.name);
                }

                this->on_event(pdb_load_event{
                    .status = pdb_load_status::mismatch,
                    .module = mod.name,
                    .path = candidate.path,
                    .origin = candidate.source,
                    .error = {},
                });
                continue;
            }

            merge_symbols(result, mod, pdb);
            this->on_event(pdb_load_event{
                .status = pdb_load_status::loaded,
                .module = mod.name,
                .path = candidate.path,
                .origin = candidate.source,
                .error = {},
            });
        }

        return result;
    }

} // namespace sogen::symbols

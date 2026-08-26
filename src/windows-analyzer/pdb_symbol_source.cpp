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

namespace sogen
{
    namespace
    {
        using namespace std::literals;

        enum class pdb_resolution_source
        {
            explicit_path,
            recorded_path,
            sibling_path,
            cache,
            symbol_store,
            download,
        };

        struct resolved_pdb
        {
            std::filesystem::path path{};
            bool explicit_input{};
            pdb_resolution_source source{};
            bool temporary{};
        };

        bool pdb_validation_failed(const pdb_validation_status status)
        {
            return status == pdb_validation_status::invalid_artifact || status == pdb_validation_status::validation_error;
        }

        bool is_pdb_path(const std::filesystem::path& path)
        {
            return utils::string::equals_ignore_case(path.extension().string(), ".pdb"s);
        }

        const char* source_label(const pdb_resolution_source source)
        {
            switch (source)
            {
            case pdb_resolution_source::explicit_path:
                return "disk explicit PDB";
            case pdb_resolution_source::recorded_path:
                return "disk recorded PDB path";
            case pdb_resolution_source::sibling_path:
                return "disk sibling PDB";
            case pdb_resolution_source::cache:
                return "disk symbol cache";
            case pdb_resolution_source::symbol_store:
                return "disk symbol store";
            case pdb_resolution_source::download:
                return "downloaded PDB";
            }

            return "PDB";
        }

        pdb_resolution_source resolution_source(const symbol_server::source source)
        {
            switch (source)
            {
            case symbol_server::source::cache:
                return pdb_resolution_source::cache;
            case symbol_server::source::symbol_store:
                return pdb_resolution_source::symbol_store;
            case symbol_server::source::download:
                return pdb_resolution_source::download;
            }

            return pdb_resolution_source::download;
        }

        std::optional<resolved_pdb> resolve_from_signature(const pdb_signature& sig, const pdb_symbol_options& options, const logger& log,
                                                           const std::string_view module_name,
                                                           const std::optional<std::filesystem::path>& module_pe_path = std::nullopt)
        {
            if (!sig.valid() || sig.path.empty())
            {
                return std::nullopt;
            }

            const std::filesystem::path recorded_path{sig.path};
            if (recorded_path.is_absolute() && utils::io::file_exists(recorded_path))
            {
                const auto validation = pdb_file{recorded_path}.validate(sig);
                if (validation.status == pdb_validation_status::match)
                {
                    return resolved_pdb{
                        .path = recorded_path,
                        .explicit_input = false,
                        .source = pdb_resolution_source::recorded_path,
                    };
                }
                if (pdb_validation_failed(validation.status))
                {
                    log.warn("Failed to validate recorded PDB for %.*s at %s: %s\n", static_cast<int>(module_name.size()),
                             module_name.data(), recorded_path.string().c_str(), validation.error.c_str());
                }
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
                        log.warn("Failed to validate sibling PDB for %.*s at %s: %s\n", static_cast<int>(module_name.size()),
                                 module_name.data(), candidate.string().c_str(), validation.error.c_str());
                        continue;
                    }

                    return resolved_pdb{
                        .path = candidate,
                        .explicit_input = false,
                        .source = pdb_resolution_source::sibling_path,
                    };
                }
            }

            if (const auto cached = symbol_cache::find(sig, options.symbol_cache, log, module_name))
            {
                return resolved_pdb{
                    .path = *cached,
                    .explicit_input = false,
                    .source = pdb_resolution_source::cache,
                };
            }

            if (const auto resolved = symbol_server::find(sig, options, log, module_name))
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

        std::optional<resolved_pdb> resolve_pe_input(const std::filesystem::path& pe_path, const pdb_symbol_options& options,
                                                     const logger& log)
        {
            auto sig = winpe::read_pdb_signature(pe_path);
            if (!sig)
            {
                throw std::runtime_error("No RSDS PDB reference found in " + pe_path.string());
            }

            std::vector<std::pair<std::filesystem::path, pdb_resolution_source>> direct_candidates{};
            const std::filesystem::path recorded_path{sig->path};
            if (recorded_path.is_absolute())
            {
                direct_candidates.emplace_back(recorded_path, pdb_resolution_source::recorded_path);
            }

            auto sibling_pdb = pe_path;
            sibling_pdb.replace_extension(".pdb");
            direct_candidates.emplace_back(std::move(sibling_pdb), pdb_resolution_source::sibling_path);
            if (!sig->filename().empty())
            {
                direct_candidates.emplace_back(pe_path.parent_path() / sig->filename(), pdb_resolution_source::sibling_path);
            }

            for (const auto& [candidate, source] : direct_candidates)
            {
                if (!utils::io::file_exists(candidate))
                {
                    continue;
                }

                const auto validation = pdb_file{candidate}.validate(*sig);
                if (validation.status == pdb_validation_status::mismatch)
                {
                    throw std::runtime_error("PDB signature mismatch for " + candidate.string());
                }
                if (validation.status != pdb_validation_status::match)
                {
                    throw std::runtime_error("Failed to validate PDB " + candidate.string() + ": " + validation.error);
                }

                return resolved_pdb{
                    .path = candidate,
                    .explicit_input = true,
                    .source = source,
                };
            }

            if (const auto resolved = resolve_from_signature(*sig, options, log, pe_path.filename().string(), pe_path))
            {
                auto pdb = *resolved;
                pdb.explicit_input = true;
                return pdb;
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
        if (this->options_.enabled())
        {
            if (!pdb_file::check_pdbutil_available())
            {
                throw std::runtime_error("PDB support requires llvm-pdbutil");
            }
        }
        symbol_cache::initialize(win_emu.log, this->options_.symbol_cache);
    }

    loaded_symbols pdb_symbol_source::load(const mapped_module& mod) const
    {
        loaded_symbols result{};
        if (!this->options_.enabled())
        {
            return result;
        }

        std::vector<resolved_pdb> candidates{};

        for (const auto& input : this->options_.inputs)
        {
            if (is_pdb_path(input))
            {
                candidates.emplace_back(resolved_pdb{
                    .path = input,
                    .explicit_input = true,
                    .source = pdb_resolution_source::explicit_path,
                });
                continue;
            }

            if (auto resolved = resolve_pe_input(input, this->options_, this->win_emu_->log))
            {
                candidates.push_back(std::move(*resolved));
            }
        }

        if (this->options_.auto_lookup && mod.pdb)
        {
            const auto module_pe_path =
                (!mod.path.empty() && utils::io::file_exists(mod.path)) ? std::optional<std::filesystem::path>{mod.path} : std::nullopt;

            if (const auto resolved = resolve_from_signature(*mod.pdb, this->options_, this->win_emu_->log, mod.name, module_pe_path))
            {
                candidates.emplace_back(*resolved);
            }
            else
            {
                this->win_emu_->log.warn("No usable PDB found for %s in symbol caches or symbol servers\n", mod.name.c_str());
            }
        }
        else if (this->options_.auto_lookup)
        {
            this->win_emu_->log.info("Skipping automatic PDB lookup for %s: no RSDS reference\n", mod.name.c_str());
        }

        if (candidates.empty())
        {
            this->win_emu_->log.info("No PDB candidates for %s\n", mod.name.c_str());
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

            this->win_emu_->log.info("Loading PDB symbols for %s from %s: %s\n", mod.name.c_str(), source_label(candidate.source),
                                     candidate.path.string().c_str());

            pdb_file pdb{candidate.path};
            const auto read_result = pdb.read();
            if (!read_result.success)
            {
                if (candidate.explicit_input)
                {
                    throw std::runtime_error("Failed to parse PDB " + candidate.path.string() + ": " + read_result.error);
                }

                this->win_emu_->log.warn("Failed to parse PDB %s: %s\n", candidate.path.string().c_str(), read_result.error.c_str());
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

                this->win_emu_->log.info("Skipping PDB %s for %s: signature mismatch\n", candidate.path.string().c_str(), mod.name.c_str());
                continue;
            }

            merge_symbols(result, mod, pdb);
            this->win_emu_->log.info("Loaded PDB symbols for %s from %s\n", mod.name.c_str(), candidate.path.string().c_str());
        }

        return result;
    }

} // namespace sogen

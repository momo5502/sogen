#pragma once

#include <utils/function.hpp>
#include <filesystem>
#include <string_view>
#include <string>
#include <variant>

namespace sogen::symbols
{
    enum class map_load_status
    {
        started,
        loaded,
        failed
    };

    enum class pdb_load_status
    {
        started,
        loaded,
        parse_failed,
        mismatch,
        source_failed
    };

    enum class pdb_origin
    {
        explicit_path,
        sibling_path,
        cache,
        symbol_store,
        download
    };

    enum class pdb_lookup_result
    {
        found,
        not_found,
        no_reference,
        no_candidates,
        invalid_filename,
        candidate_validation_failed
    };

    enum class cache_status
    {
        cleanup_failed,
        removing,
        removal_failed
    };

    enum class download_status
    {
        started,
        not_found,
        failed
    };

    enum class artifact_error
    {
        mismatch,
        invalid,
        cache_failed,
        create_file_failed,
        decompression_failed,
        store_validation_failed
    };

    struct map_load_event
    {
        map_load_status status;
        std::string module;
        std::filesystem::path path;
        std::string error;
    };

    struct pdb_lookup_event
    {
        pdb_lookup_result status;
        std::string module;
        std::filesystem::path path{};
        std::string error{};
    };

    struct pdb_load_event
    {
        pdb_load_status status;
        std::string module;
        std::filesystem::path path;
        pdb_origin origin;
        std::string error;
    };

    struct cache_validation_event
    {
        std::string module;
        std::filesystem::path path;
        std::string error;
    };

    struct cache_maintenance_event
    {
        cache_status status;
        std::string module;
        std::filesystem::path path;
        std::string error;
    };

    struct symbol_download_event
    {
        download_status status;
        std::string module;
        std::string server;
        std::string error;
    };

    struct artifact_processing_event
    {
        artifact_error status;
        std::string module;
        std::string server;
        std::string error;
    };

    using symbol_event = std::variant<map_load_event, pdb_lookup_event, pdb_load_event, cache_validation_event, cache_maintenance_event,
                                      symbol_download_event, artifact_processing_event>;
    using symbol_event_callback = utils::optional_function<void(const symbol_event&)>;
}

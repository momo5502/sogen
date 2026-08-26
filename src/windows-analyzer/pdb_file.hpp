#pragma once

#include <platform/pdb_signature.hpp>

#include <map>
#include <string>
#include <utility>

namespace sogen
{
    enum class pdb_validation_status
    {
        match,
        mismatch,
        invalid_artifact,
        validation_error,
    };

    struct pdb_validation_result
    {
        pdb_validation_status status{};
        std::string error{};
    };

    class pdb_file
    {
      public:
        pdb_signature signature{};
        std::map<std::pair<uint32_t, uint32_t>, std::string> symbols{};

        static pdb_file read(const std::filesystem::path& path);
        static pdb_validation_result validate(const std::filesystem::path& path, const pdb_signature& expected);
        static bool signatures_match(const pdb_signature& lhs, const pdb_signature& rhs);
    };
}

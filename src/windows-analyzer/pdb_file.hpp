#pragma once

#include <platform/pdb_signature.hpp>

#include <filesystem>
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

    struct pdb_read_result
    {
        bool success{};
        std::string error{};
    };

    class pdb_file
    {
      public:
        explicit pdb_file(std::filesystem::path path);

        pdb_signature signature{};
        std::map<std::pair<uint32_t, uint32_t>, std::string> symbols{};

        pdb_read_result read();
        pdb_validation_result validate(const pdb_signature& expected) const;
        bool matches(const pdb_signature& expected) const;

        static bool check_pdbutil_available();

      private:
        std::filesystem::path path_{};
    };
}

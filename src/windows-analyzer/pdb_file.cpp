#include "pdb_file.hpp"

#include <utils/exec.hpp>
#include <utils/string.hpp>
#include <platform/platform.hpp>
#include <platform/win_pefile_debug.hpp>

#include <charconv>
#include <optional>
#include <sstream>
#include <vector>

namespace sogen
{
    namespace
    {
        const std::string& pdbutil_command()
        {
            static const std::string command = [] {
#if (defined(__linux__) || defined(__APPLE__)) && !defined(__EMSCRIPTEN__)
                std::vector<std::string> names{"llvm-pdbutil"};

#if defined(__APPLE__)
                names.emplace_back("/opt/homebrew/opt/llvm/bin/llvm-pdbutil");
                names.emplace_back("/usr/local/opt/llvm/bin/llvm-pdbutil");
#endif

                for (int version = 14; version <= 30; ++version)
                {
                    names.emplace_back("llvm-pdbutil-" + std::to_string(version));
                }

                for (const auto& name : names)
                {
                    try
                    {
                        auto candidate = utils::string::trim(utils::exec::run_command_capture({"which", name}));
                        if (!candidate.empty())
                        {
                            return candidate;
                        }
                    }
                    catch (const std::exception&)
                    {
                        continue;
                    }
                }
#endif

                return std::string{"llvm-pdbutil"};
            }();

            return command;
        }

        std::optional<std::pair<uint32_t, uint32_t>> parse_section_offset(const std::string_view line)
        {
            const auto addr_pos = line.find("addr = ");
            if (addr_pos == std::string_view::npos)
            {
                return std::nullopt;
            }

            auto value = line.substr(addr_pos + 7);
            const auto comma = value.find(',');
            if (comma != std::string_view::npos)
            {
                value = value.substr(0, comma);
            }

            const auto colon = value.find(':');
            if (colon == std::string_view::npos)
            {
                return std::nullopt;
            }

            uint32_t section{};
            uint32_t offset{};
            const auto section_text = utils::string::trim(value.substr(0, colon));
            const auto offset_text = utils::string::trim(value.substr(colon + 1));

            auto result = std::from_chars(section_text.data(), section_text.data() + section_text.size(), section, 10);
            if (result.ec != std::errc{})
            {
                return std::nullopt;
            }

            result = std::from_chars(offset_text.data(), offset_text.data() + offset_text.size(), offset, 10);
            if (result.ec != std::errc{})
            {
                return std::nullopt;
            }

            return std::pair{section, offset};
        }

        std::optional<std::string> extract_backtick_name(const std::string_view text)
        {
            const auto first = text.find('`');
            const auto last = text.rfind('`');
            if (first == std::string::npos || last == first)
            {
                return std::nullopt;
            }

            return std::string{text.substr(first + 1, last - first - 1)};
        }

        std::optional<uint32_t> parse_age(const std::string_view value)
        {
            uint32_t age{};
            const auto text = utils::string::trim(value);
            const auto result = std::from_chars(text.data(), text.data() + text.size(), age, 10);
            if (result.ec != std::errc{})
            {
                return std::nullopt;
            }

            return age;
        }

        void parse_pdb_guid_line(const std::string_view trimmed, pdb_signature& signature)
        {
            if (trimmed.starts_with("GUID:"))
            {
                signature.guid = winpe::detail::normalize_guid(utils::string::trim(trimmed.substr(5)));
            }
        }

        std::optional<uint32_t> read_pdb_dbi_age(const std::filesystem::path& path)
        {
            const auto output = utils::exec::run_command_capture({pdbutil_command(), "pdb2yaml", "-dbi-stream", path.string()});

            std::istringstream stream{output};
            std::string line{};
            bool parsing_dbi_stream = false;
            while (std::getline(stream, line))
            {
                const auto trimmed = utils::string::trim(line);
                if (trimmed == "DbiStream:")
                {
                    parsing_dbi_stream = true;
                    continue;
                }

                if (parsing_dbi_stream && trimmed.starts_with("Age:"))
                {
                    return parse_age(trimmed.substr(4));
                }
            }

            return std::nullopt;
        }

        std::optional<pdb_signature> read_pdb_signature(const std::filesystem::path& path)
        {
            const auto output = utils::exec::run_command_capture({pdbutil_command(), "dump", "-summary", path.string()});

            pdb_signature signature{};
            std::istringstream stream{output};
            std::string line{};
            while (std::getline(stream, line))
            {
                parse_pdb_guid_line(utils::string::trim(line), signature);
            }

            signature.age = read_pdb_dbi_age(path).value_or(0);
            if (!signature.valid())
            {
                return std::nullopt;
            }

            signature.path = path.string();
            return signature;
        }
    }

    pdb_file pdb_file::read(const std::filesystem::path& path)
    {
        const auto output =
            utils::exec::run_command_capture({pdbutil_command(), "dump", "-summary", "-publics", "-symbols", path.string()});

        pdb_file pdb{};
        std::optional<std::string> pending_public{};
        std::optional<std::string> pending_proc{};
        std::string pending_proc_text{};

        std::istringstream stream{output};
        std::string line{};
        while (std::getline(stream, line))
        {
            const auto trimmed = utils::string::trim(line);

            parse_pdb_guid_line(trimmed, pdb.signature);

            if (trimmed.find("S_PUB32") != std::string::npos)
            {
                pending_public = extract_backtick_name(trimmed);
                continue;
            }

            if (pending_public)
            {
                if (trimmed.find("flags = function") != std::string::npos)
                {
                    if (const auto address = parse_section_offset(trimmed))
                    {
                        pdb.symbols.try_emplace(*address, *pending_public);
                    }
                }

                pending_public.reset();
                continue;
            }

            if (trimmed.find("S_GPROC32") != std::string::npos || trimmed.find("S_LPROC32") != std::string::npos)
            {
                pending_proc_text.assign(trimmed);
                pending_proc = extract_backtick_name(pending_proc_text);
                continue;
            }

            if (!pending_proc_text.empty())
            {
                pending_proc_text += ' ';
                pending_proc_text.append(trimmed);
                if (!pending_proc)
                {
                    pending_proc = extract_backtick_name(pending_proc_text);
                }

                if (trimmed.find("addr = ") != std::string::npos)
                {
                    if (pending_proc)
                    {
                        if (const auto address = parse_section_offset(trimmed))
                        {
                            pdb.symbols[*address] = *pending_proc;
                        }
                    }

                    pending_proc.reset();
                    pending_proc_text.clear();
                }
            }
        }

        pdb.signature.age = read_pdb_dbi_age(path).value_or(0);
        if (!pdb.signature.valid())
        {
            throw std::runtime_error("llvm-pdbutil did not report a valid PDB identity for " + path.string());
        }

        pdb.signature.path = path.string();
        return pdb;
    }

    void pdb_file::ensure_available()
    {
        static const bool available = [] {
            try
            {
                (void)utils::exec::run_command_capture({pdbutil_command(), "--version"});
            }
            catch (const std::exception& e)
            {
                throw std::runtime_error("PDB support requires llvm-pdbutil: " + std::string{e.what()});
            }
            return true;
        }();
        (void)available;
    }

    pdb_validation_result pdb_file::validate(const std::filesystem::path& path, const pdb_signature& expected)
    {
        try
        {
            const auto actual = read_pdb_signature(path);
            if (!actual)
            {
                return {.status = pdb_validation_status::invalid_artifact, .error = "llvm-pdbutil did not report a valid PDB identity"};
            }

            return {.status = signatures_match(*actual, expected) ? pdb_validation_status::match : pdb_validation_status::mismatch};
        }
        catch (const utils::exec::external_command_exit_error& e)
        {
            return {.status = pdb_validation_status::invalid_artifact, .error = std::string{e.what()}};
        }
        catch (const std::exception& e)
        {
            return {.status = pdb_validation_status::validation_error, .error = std::string{e.what()}};
        }
    }

    bool pdb_file::signatures_match(const pdb_signature& lhs, const pdb_signature& rhs)
    {
        return winpe::detail::normalize_guid(lhs.guid) == winpe::detail::normalize_guid(rhs.guid) && lhs.age == rhs.age;
    }
}

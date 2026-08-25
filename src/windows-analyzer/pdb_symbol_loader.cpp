#include "pdb_symbol_loader.hpp"

#include <windows_emulator.hpp>
#include <utils/buffer_accessor.hpp>
#include <utils/io.hpp>
#include <utils/string.hpp>
#include <platform/win_pefile.hpp>

#include <cstdlib>
#include <iomanip>
#include <regex>
#include <sstream>

#ifdef _WIN32
#include <Windows.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace sogen
{
    namespace
    {
        constexpr auto k_default_symbol_server = "https://msdl.microsoft.com/download/symbols"sv;
        constexpr auto k_sogen_cache_marker = ".sogen-symbol-cache"sv;
        constexpr uint32_t k_image_debug_type_codeview = 2;
        constexpr auto k_cache_max_age = std::chrono::hours{24 * 31};

        struct image_debug_directory
        {
            uint32_t characteristics{};
            uint32_t time_date_stamp{};
            uint16_t major_version{};
            uint16_t minor_version{};
            uint32_t type{};
            uint32_t size_of_data{};
            uint32_t address_of_raw_data{};
            uint32_t pointer_to_raw_data{};
        };

        struct parsed_pdb
        {
            pdb_signature signature{};
            std::map<std::pair<uint32_t, uint32_t>, std::string> symbols{};
        };

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
            std::string source_detail{};
        };

        struct download_result
        {
            bool success{};
            std::string reason{};
            bool not_found{};
            bool timeout{};
        };

        std::string trim(std::string value)
        {
            const auto begin = value.find_first_not_of(" \t\r\n");
            if (begin == std::string::npos)
            {
                return {};
            }

            const auto end = value.find_last_not_of(" \t\r\n");
            return value.substr(begin, end - begin + 1);
        }

        bool is_pdb_path(const std::filesystem::path& path)
        {
            return utils::string::equals_ignore_case(path.extension().string(), ".pdb"s);
        }

        bool is_http_server(const std::string_view value)
        {
            return utils::string::starts_with_ignore_case(value, "http://"sv) ||
                   utils::string::starts_with_ignore_case(value, "https://"sv);
        }

        std::string single_line(std::string value)
        {
            for (auto& ch : value)
            {
                if (ch == '\r' || ch == '\n' || ch == '\t')
                {
                    ch = ' ';
                }
            }

            value = trim(std::move(value));
            constexpr size_t max_len = 300;
            if (value.size() > max_len)
            {
                value.resize(max_len);
                value += "...";
            }

            return value;
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

        std::string normalize_guid(const std::string_view value)
        {
            std::string normalized{};
            normalized.reserve(32);

            for (const auto ch : value)
            {
                if (std::isxdigit(static_cast<unsigned char>(ch)))
                {
                    normalized.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(ch))));
                }
            }

            return normalized;
        }

        std::string make_signature_key(const pdb_signature& sig)
        {
            std::ostringstream stream{};
            stream << normalize_guid(sig.guid) << std::nouppercase << std::hex << sig.age;
            return stream.str();
        }

        std::string format_guid(const std::span<const std::byte, 16> guid)
        {
            uint32_t data1{};
            uint16_t data2{};
            uint16_t data3{};
            memcpy(&data1, guid.data(), sizeof(data1));
            memcpy(&data2, guid.data() + 4, sizeof(data2));
            memcpy(&data3, guid.data() + 6, sizeof(data3));

            std::ostringstream stream{};
            stream << std::uppercase << std::hex << std::setfill('0') << std::setw(8) << data1 << std::setw(4) << data2 << std::setw(4)
                   << data3;

            for (size_t i = 8; i < guid.size(); ++i)
            {
                stream << std::setw(2) << std::to_integer<unsigned>(guid[i]);
            }

            return stream.str();
        }

        std::optional<pdb_signature> parse_codeview_record(const utils::safe_buffer_accessor<const std::byte>& buffer, const size_t offset,
                                                           const size_t size)
        {
            if (size < 24)
            {
                return std::nullopt;
            }

            const auto* data = buffer.get_pointer_for_range(offset, size);
            if (memcmp(data, "RSDS", 4) != 0)
            {
                return std::nullopt;
            }

            std::span<const std::byte, 16> guid{data + 4, 16};
            uint32_t age{};
            memcpy(&age, data + 20, sizeof(age));

            std::string path{};
            for (size_t i = 24; i < size && data[i] != std::byte{}; ++i)
            {
                path.push_back(static_cast<char>(data[i]));
            }

            pdb_signature sig{};
            sig.guid = format_guid(guid);
            sig.age = age;
            sig.path = std::move(path);
            return sig;
        }

        template <typename T>
        std::optional<size_t> rva_to_file_offset(const utils::safe_buffer_accessor<const std::byte>& buffer,
                                                 const PENTHeaders_t<T>& nt_headers, const uint64_t nt_headers_offset, const uint32_t rva)
        {
            if (rva < nt_headers.OptionalHeader.SizeOfHeaders)
            {
                return rva;
            }

            const auto section_offset = winpe::get_first_section_offset(nt_headers, nt_headers_offset);
            const auto sections = buffer.as<IMAGE_SECTION_HEADER>(static_cast<size_t>(section_offset));

            for (size_t i = 0; i < nt_headers.FileHeader.NumberOfSections; ++i)
            {
                const auto section = sections.get(i);
                const auto size = std::max(section.Misc.VirtualSize, section.SizeOfRawData);
                if (rva >= section.VirtualAddress && rva < section.VirtualAddress + size)
                {
                    return section.PointerToRawData + (rva - section.VirtualAddress);
                }
            }

            return std::nullopt;
        }

        template <typename T>
        std::optional<pdb_signature> read_pe_pdb_signature(const utils::safe_buffer_accessor<const std::byte>& buffer,
                                                           const PENTHeaders_t<T>& nt_headers, const uint64_t nt_headers_offset)
        {
            const auto& debug_entry = nt_headers.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG];
            if (debug_entry.VirtualAddress == 0 || debug_entry.Size < sizeof(image_debug_directory))
            {
                return std::nullopt;
            }

            const auto debug_offset = rva_to_file_offset(buffer, nt_headers, nt_headers_offset, debug_entry.VirtualAddress);
            if (!debug_offset)
            {
                return std::nullopt;
            }

            const auto debug_directories = buffer.as<image_debug_directory>(*debug_offset);
            const auto count = debug_entry.Size / sizeof(image_debug_directory);

            for (size_t i = 0; i < count; ++i)
            {
                const auto debug = debug_directories.get(i);
                if (debug.type != k_image_debug_type_codeview || debug.size_of_data == 0)
                {
                    continue;
                }

                auto sig = parse_codeview_record(buffer, debug.pointer_to_raw_data, debug.size_of_data);
                if (sig)
                {
                    return sig;
                }
            }

            return std::nullopt;
        }

        std::optional<pdb_signature> read_pe_pdb_signature(const std::filesystem::path& file)
        {
            const auto data = utils::io::read_file(file);
            if (data.empty())
            {
                return std::nullopt;
            }

            utils::safe_buffer_accessor<const std::byte> buffer{data};

            try
            {
                const auto dos_header = buffer.as<PEDosHeader_t>(0).get();
                const auto nt_headers_offset = dos_header.e_lfanew;

                const auto nt_signature = buffer.as<uint32_t>(nt_headers_offset).get();
                if (dos_header.e_magic != PEDosHeader_t::k_Magic || nt_signature != PENTHeaders_t<uint32_t>::k_Signature)
                {
                    return std::nullopt;
                }

                const auto magic_offset = nt_headers_offset + sizeof(uint32_t) + sizeof(PEFileHeader_t);
                const auto magic = buffer.as<uint16_t>(magic_offset).get();
                if (magic == PEOptionalHeader_t<uint32_t>::k_Magic)
                {
                    const auto nt_headers = buffer.as<PENTHeaders_t<uint32_t>>(nt_headers_offset).get();
                    return read_pe_pdb_signature(buffer, nt_headers, nt_headers_offset);
                }

                if (magic == PEOptionalHeader_t<uint64_t>::k_Magic)
                {
                    const auto nt_headers = buffer.as<PENTHeaders_t<uint64_t>>(nt_headers_offset).get();
                    return read_pe_pdb_signature(buffer, nt_headers, nt_headers_offset);
                }
            }
            catch (...)
            {
            }

            return std::nullopt;
        }

        std::optional<uint32_t> section_offset_to_rva(const mapped_module& mod, const uint32_t section_index, const uint32_t offset)
        {
            if (section_index == 0 || section_index > mod.pe_sections.size())
            {
                return std::nullopt;
            }

            const auto& section = mod.pe_sections[section_index - 1];
            const auto section_size = std::max(section.virtual_size, section.raw_size);
            if (section_size != 0 && offset > section_size)
            {
                return std::nullopt;
            }

            const auto rva = section.virtual_address + offset;
            if (rva >= mod.size_of_image)
            {
                return std::nullopt;
            }

            return rva;
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
            const auto section_text = trim(std::string(value.substr(0, colon)));
            const auto offset_text = trim(std::string(value.substr(colon + 1)));

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

        std::optional<std::string> extract_backtick_name(const std::string& text)
        {
            const auto first = text.find('`');
            const auto last = text.rfind('`');
            if (first == std::string::npos || last == first)
            {
                return std::nullopt;
            }

            return text.substr(first + 1, last - first - 1);
        }

        std::optional<uint32_t> parse_age(const std::string_view value)
        {
            uint32_t age{};
            auto text = trim(std::string(value));
            const auto result = std::from_chars(text.data(), text.data() + text.size(), age, 10);
            if (result.ec != std::errc{})
            {
                return std::nullopt;
            }

            return age;
        }

        std::string quote_command_arg(const std::string& arg)
        {
            if (arg.find('"') != std::string::npos)
            {
                throw std::runtime_error("External command arguments containing quotes are not supported: " + arg);
            }

            if (arg.find_first_of(" \t&|<>^") == std::string::npos)
            {
                return arg;
            }

            return "\"" + arg + "\"";
        }

        std::string run_command_capture(const std::vector<std::string>& args)
        {
            if (args.empty())
            {
                return {};
            }

            std::string command{};
            for (const auto& arg : args)
            {
                if (!command.empty())
                {
                    command.push_back(' ');
                }

                command += quote_command_arg(arg);
            }

            command += " 2>&1";

#ifdef _WIN32
            auto* pipe = _popen(command.c_str(), "r");
#else
            auto* pipe = popen(command.c_str(), "r");
#endif
            if (!pipe)
            {
                throw std::runtime_error("Failed to execute external command: " + args.front());
            }

            std::string output{};
            std::array<char, 4096> buffer{};
            while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe))
            {
                output.append(buffer.data());
            }

#ifdef _WIN32
            const auto status = _pclose(pipe);
#else
            const auto status = pclose(pipe);
#endif
            if (status != 0)
            {
                throw std::runtime_error("External command failed: " + args.front() + "\n" + output);
            }

            return output;
        }

        void parse_pdb_signature_line(const std::string_view trimmed, pdb_signature& signature)
        {
            if (trimmed.starts_with("GUID:"))
            {
                signature.guid = normalize_guid(trim(std::string(trimmed.substr(5))));
                return;
            }

            if (trimmed.starts_with("Age:"))
            {
                if (const auto age = parse_age(trimmed.substr(4)))
                {
                    signature.age = *age;
                }
            }
        }

        std::optional<pdb_signature> read_pdb_signature(const std::filesystem::path& pdb_path)
        {
            const auto output = run_command_capture({"llvm-pdbutil", "dump", "-summary", pdb_path.string()});

            pdb_signature signature{};
            std::istringstream stream{output};
            std::string line{};
            while (std::getline(stream, line))
            {
                parse_pdb_signature_line(trim(line), signature);
            }

            if (!signature.valid())
            {
                return std::nullopt;
            }

            signature.path = pdb_path;
            return signature;
        }

        bool pdb_matches_signature(const std::filesystem::path& pdb_path, const pdb_signature& expected)
        {
            try
            {
                const auto actual = read_pdb_signature(pdb_path);
                return actual && normalize_guid(actual->guid) == normalize_guid(expected.guid);
            }
            catch (...)
            {
                return false;
            }
        }

        parsed_pdb parse_pdb(const std::filesystem::path& pdb_path)
        {
            const auto output = run_command_capture({"llvm-pdbutil", "dump", "-summary", "-publics", "-symbols", pdb_path.string()});

            parsed_pdb pdb{};
            std::optional<std::string> pending_public{};
            std::optional<std::string> pending_proc{};
            std::string pending_proc_text{};

            std::istringstream stream{output};
            std::string line{};
            while (std::getline(stream, line))
            {
                const auto trimmed = trim(line);

                parse_pdb_signature_line(trimmed, pdb.signature);

                if (trimmed.find("S_PUB32") != std::string::npos)
                {
                    pending_public = extract_backtick_name(trimmed);
                    continue;
                }

                if (pending_public)
                {
                    if (trimmed.find("flags = function") != std::string::npos)
                    {
                        if (const auto addr = parse_section_offset(trimmed))
                        {
                            pdb.symbols.try_emplace(*addr, *pending_public);
                        }
                    }

                    pending_public.reset();
                    continue;
                }

                if (trimmed.find("S_GPROC32") != std::string::npos || trimmed.find("S_LPROC32") != std::string::npos)
                {
                    pending_proc_text = trimmed;
                    pending_proc = extract_backtick_name(pending_proc_text);
                    continue;
                }

                if (!pending_proc_text.empty())
                {
                    pending_proc_text += " " + trimmed;
                    if (!pending_proc)
                    {
                        pending_proc = extract_backtick_name(pending_proc_text);
                    }

                    if (trimmed.find("addr = ") != std::string::npos)
                    {
                        if (pending_proc)
                        {
                            if (const auto addr = parse_section_offset(trimmed))
                            {
                                pdb.symbols[*addr] = *pending_proc;
                            }
                        }

                        pending_proc.reset();
                        pending_proc_text.clear();
                    }
                }
            }

            if (!pdb.signature.valid())
            {
                throw std::runtime_error("llvm-pdbutil did not report a valid PDB signature for " + pdb_path.string());
            }

            pdb.signature.path = pdb_path;
            return pdb;
        }

        std::filesystem::path default_sogen_cache_root()
        {
#ifdef _WIN32
            if (const auto* local_app_data = std::getenv("LOCALAPPDATA"))
            {
                return std::filesystem::path(local_app_data) / "sogen" / "symbols";
            }

            if (const auto* user_profile = std::getenv("USERPROFILE"))
            {
                return std::filesystem::path(user_profile) / "AppData" / "Local" / "sogen" / "symbols";
            }
#else
            if (const auto* xdg_cache = std::getenv("XDG_CACHE_HOME"))
            {
                return std::filesystem::path(xdg_cache) / "sogen" / "symbols";
            }

            if (const auto* home = std::getenv("HOME"))
            {
                return std::filesystem::path(home) / ".cache" / "sogen" / "symbols";
            }
#endif
            return std::filesystem::current_path() / ".sogen-symbols";
        }

        std::vector<std::string> split_symbol_path(const std::string_view value)
        {
            std::vector<std::string> entries{};
            size_t start = 0;

            for (size_t i = 0; i <= value.size(); ++i)
            {
                if (i != value.size() && value[i] != ';')
                {
                    continue;
                }

                auto entry = trim(std::string(value.substr(start, i - start)));
                if (!entry.empty())
                {
                    entries.push_back(std::move(entry));
                }
                start = i + 1;
            }

            return entries;
        }

        void add_unique_path(std::vector<std::filesystem::path>& paths, std::filesystem::path path)
        {
            if (path.empty())
            {
                return;
            }

            std::error_code ec{};
            if (!std::filesystem::exists(path, ec) || ec)
            {
                return;
            }

            path = std::filesystem::absolute(path, ec);
            if (ec)
            {
                return;
            }

            for (const auto& existing : paths)
            {
                if (utils::string::equals_ignore_case(existing.string(), path.string()))
                {
                    return;
                }
            }

            paths.push_back(std::move(path));
        }

        void collect_nt_symbol_path_caches(std::vector<std::filesystem::path>& paths)
        {
            const auto* env = std::getenv("_NT_SYMBOL_PATH");
            if (!env)
            {
                return;
            }

            for (const auto& entry : split_symbol_path(env))
            {
                if (utils::string::starts_with_ignore_case(std::string_view{entry}, "cache*"sv))
                {
                    add_unique_path(paths, entry.substr(6));
                    continue;
                }

                if (utils::string::starts_with_ignore_case(std::string_view{entry}, "srv*"sv))
                {
                    const auto first = entry.find('*');
                    if (first == std::string::npos)
                    {
                        continue;
                    }

                    const auto second = entry.find('*', first + 1);
                    if (second == std::string::npos)
                    {
                        continue;
                    }

                    const auto cache = entry.substr(first + 1, second - first - 1);
                    if (!cache.empty() && !is_http_server(cache))
                    {
                        add_unique_path(paths, cache);
                    }
                    continue;
                }

                if (!is_http_server(entry))
                {
                    add_unique_path(paths, entry);
                }
            }
        }

        std::vector<std::filesystem::path> existing_cache_roots(const std::optional<std::filesystem::path>& explicit_cache)
        {
            std::vector<std::filesystem::path> paths{};

            if (explicit_cache)
            {
                add_unique_path(paths, *explicit_cache);
            }

            collect_nt_symbol_path_caches(paths);

#ifdef _WIN32
            add_unique_path(paths, "C:\\Symbols");
            add_unique_path(paths, "C:\\SymCache");
#endif

            add_unique_path(paths, default_sogen_cache_root());
            return paths;
        }

        std::filesystem::path symbol_store_path(const std::filesystem::path& root, const pdb_signature& sig)
        {
            const auto pdb_name = sig.path.filename();
            return root / pdb_name / make_signature_key(sig) / pdb_name;
        }

        std::optional<resolved_pdb> find_in_caches(const pdb_signature& sig, const std::optional<std::filesystem::path>& explicit_cache)
        {
            for (const auto& root : existing_cache_roots(explicit_cache))
            {
                const auto path = symbol_store_path(root, sig);
                if (utils::io::file_exists(path) && pdb_matches_signature(path, sig))
                {
                    return resolved_pdb{
                        .path = path,
                        .explicit_input = false,
                        .source = pdb_resolution_source::cache,
                        .source_detail = root.string(),
                    };
                }
            }

            return std::nullopt;
        }

        std::string join_url(const std::string& server, const pdb_signature& sig)
        {
            std::string result = server;
            while (!result.empty() && result.back() == '/')
            {
                result.pop_back();
            }

            const auto pdb_name = sig.path.filename().string();
            return result + "/" + pdb_name + "/" + make_signature_key(sig) + "/" + pdb_name;
        }

        download_result download_with_curl(const std::string& url, const std::filesystem::path& target)
        {
            const auto temp = target.string() + ".tmp";
            std::error_code ec{};
            std::filesystem::create_directories(target.parent_path(), ec);
            if (ec)
            {
                return {.reason = "failed to create cache directory: " + ec.message()};
            }

            try
            {
                (void)run_command_capture({"curl", "-fsSL", "--connect-timeout", "10", "--max-time", "120", "-o", temp, url});
            }
            catch (const std::exception& e)
            {
                std::filesystem::remove(temp, ec);
                auto reason = single_line(e.what());
                const auto not_found = reason.find("(22)") != std::string::npos && reason.find("404") != std::string::npos;
                const auto timeout = reason.find("(28)") != std::string::npos || reason.find("timed out") != std::string::npos ||
                                     reason.find("Timeout") != std::string::npos;
                if (not_found)
                {
                    reason = "symbol not found on server";
                }
                else if (timeout)
                {
                    reason = "download timed out";
                }
                return {.reason = std::move(reason), .not_found = not_found, .timeout = timeout};
            }

            std::filesystem::rename(temp, target, ec);
            if (ec)
            {
                std::filesystem::copy_file(temp, target, std::filesystem::copy_options::overwrite_existing, ec);
                std::filesystem::remove(temp, ec);
            }

            if (!utils::io::file_exists(target))
            {
                return {.reason = "download finished but cache file was not created"};
            }

            return {.success = true};
        }

        std::optional<resolved_pdb> find_on_symbol_servers(const pdb_signature& sig, const pdb_symbol_options& options, const logger& log,
                                                           const std::string_view module_name)
        {
            auto download_root = options.symbol_cache.value_or(default_sogen_cache_root());
            const auto download_target = symbol_store_path(download_root, sig);
            if (utils::io::file_exists(download_target))
            {
                if (pdb_matches_signature(download_target, sig))
                {
                    return resolved_pdb{
                        .path = download_target,
                        .explicit_input = false,
                        .source = pdb_resolution_source::cache,
                        .source_detail = download_root.string(),
                    };
                }

                log.warn("Ignoring stale cached PDB for %.*s: %s\n", static_cast<int>(module_name.size()), module_name.data(),
                         download_target.string().c_str());
                std::error_code ec{};
                std::filesystem::remove(download_target, ec);
            }

            std::vector<std::string> servers = options.symbol_servers;
            servers.emplace_back(k_default_symbol_server);

            for (const auto& server : servers)
            {
                if (is_http_server(server))
                {
                    const auto url = join_url(server, sig);
                    log.info("Downloading PDB symbols for %.*s from %s\n", static_cast<int>(module_name.size()), module_name.data(),
                             server.c_str());
                    const auto download = download_with_curl(url, download_target);
                    if (download.success)
                    {
                        if (pdb_matches_signature(download_target, sig))
                        {
                            return resolved_pdb{
                                .path = download_target,
                                .explicit_input = false,
                                .source = pdb_resolution_source::download,
                                .source_detail = server,
                            };
                        }

                        log.warn("Downloaded PDB for %.*s did not match expected GUID: %s\n", static_cast<int>(module_name.size()),
                                 module_name.data(), download_target.string().c_str());
                        std::error_code ec{};
                        std::filesystem::remove(download_target, ec);
                        continue;
                    }

                    if (download.not_found)
                    {
                        log.info("PDB symbols for %.*s were not found on %s\n", static_cast<int>(module_name.size()), module_name.data(),
                                 server.c_str());
                    }
                    else
                    {
                        log.warn("Failed to download PDB symbols for %.*s from %s: %s\n", static_cast<int>(module_name.size()),
                                 module_name.data(), server.c_str(), download.reason.c_str());
                    }
                    continue;
                }

                const auto path = symbol_store_path(server, sig);
                if (utils::io::file_exists(path) && pdb_matches_signature(path, sig))
                {
                    return resolved_pdb{
                        .path = path,
                        .explicit_input = false,
                        .source = pdb_resolution_source::symbol_store,
                        .source_detail = server,
                    };
                }
            }

            return std::nullopt;
        }

        void ensure_default_cache_marker()
        {
            const auto root = default_sogen_cache_root();
            std::error_code ec{};
            std::filesystem::create_directories(root, ec);
            if (ec)
            {
                return;
            }

            const auto marker = root / k_sogen_cache_marker;
            if (!utils::io::file_exists(marker))
            {
                (void)utils::io::write_file(marker, std::span<const std::byte>{});
            }
        }

#ifdef _WIN32
        std::optional<std::chrono::system_clock::time_point> last_access_time(const std::filesystem::path& path)
        {
            WIN32_FILE_ATTRIBUTE_DATA data{};
            if (!GetFileAttributesExW(path.wstring().c_str(), GetFileExInfoStandard, &data))
            {
                return std::nullopt;
            }

            ULARGE_INTEGER value{};
            value.LowPart = data.ftLastAccessTime.dwLowDateTime;
            value.HighPart = data.ftLastAccessTime.dwHighDateTime;
            constexpr uint64_t windows_to_unix_epoch_100ns = 116444736000000000ull;
            if (value.QuadPart < windows_to_unix_epoch_100ns)
            {
                return std::nullopt;
            }

            const auto ns = (value.QuadPart - windows_to_unix_epoch_100ns) * 100;
            return std::chrono::system_clock::time_point{
                std::chrono::duration_cast<std::chrono::system_clock::duration>(std::chrono::nanoseconds{ns})};
        }
#else
        std::optional<std::chrono::system_clock::time_point> last_access_time(const std::filesystem::path& path)
        {
            struct stat st{};
            if (stat(path.string().c_str(), &st) != 0)
            {
                return std::nullopt;
            }

            return std::chrono::system_clock::from_time_t(st.st_atime);
        }
#endif

        void prune_sogen_cache(const logger& log, const std::optional<std::filesystem::path>& explicit_cache)
        {
            if (explicit_cache)
            {
                return;
            }

            const auto root = default_sogen_cache_root();
            if (!utils::io::file_exists(root / k_sogen_cache_marker))
            {
                return;
            }

            const auto cutoff = std::chrono::system_clock::now() - k_cache_max_age;
            std::error_code ec{};
            if (!std::filesystem::is_directory(root, ec) || ec)
            {
                return;
            }

            for (const auto& entry : std::filesystem::recursive_directory_iterator(root, ec))
            {
                if (ec || entry.path().filename() == k_sogen_cache_marker)
                {
                    continue;
                }

                const auto access_time = last_access_time(entry.path());
                if (!access_time || *access_time >= cutoff)
                {
                    continue;
                }

                if (entry.is_regular_file(ec) && !ec)
                {
                    std::filesystem::remove(entry.path(), ec);
                }
            }

            std::vector<std::filesystem::path> directories{};
            for (const auto& entry : std::filesystem::recursive_directory_iterator(root, ec))
            {
                if (!ec && entry.is_directory(ec) && !ec)
                {
                    directories.push_back(entry.path());
                }
            }

            std::ranges::sort(directories, [](const auto& lhs, const auto& rhs) { return lhs.string().size() > rhs.string().size(); });
            for (const auto& directory : directories)
            {
                if (utils::io::directory_is_empty(directory))
                {
                    std::filesystem::remove(directory, ec);
                }
            }

            if (ec)
            {
                log.warn("PDB cache cleanup encountered an error: %s\n", ec.message().c_str());
            }
        }

        std::optional<resolved_pdb> resolve_from_signature(const pdb_signature& sig, const pdb_symbol_options& options, const logger& log,
                                                           const std::string_view module_name,
                                                           const std::optional<std::filesystem::path>& module_pe_path = std::nullopt)
        {
            if (!sig.valid() || sig.path.empty())
            {
                return std::nullopt;
            }

            if (sig.path.is_absolute() && utils::io::file_exists(sig.path) && pdb_matches_signature(sig.path, sig))
            {
                return resolved_pdb{
                    .path = sig.path,
                    .explicit_input = false,
                    .source = pdb_resolution_source::recorded_path,
                    .source_detail = sig.path.parent_path().string(),
                };
            }

            if (module_pe_path && !module_pe_path->empty())
            {
                std::vector<std::filesystem::path> sibling_candidates{};

                auto sibling_pdb = *module_pe_path;
                sibling_pdb.replace_extension(".pdb");
                sibling_candidates.push_back(std::move(sibling_pdb));

                if (sig.path.has_filename())
                {
                    sibling_candidates.push_back(module_pe_path->parent_path() / sig.path.filename());
                }

                for (const auto& candidate : sibling_candidates)
                {
                    if (!utils::io::file_exists(candidate) || !pdb_matches_signature(candidate, sig))
                    {
                        continue;
                    }

                    return resolved_pdb{
                        .path = candidate,
                        .explicit_input = false,
                        .source = pdb_resolution_source::sibling_path,
                        .source_detail = candidate.parent_path().string(),
                    };
                }
            }

            if (const auto cached = find_in_caches(sig, options.symbol_cache))
            {
                return cached;
            }

            return find_on_symbol_servers(sig, options, log, module_name);
        }

        std::optional<resolved_pdb> resolve_pe_input(const std::filesystem::path& pe_path, const pdb_symbol_options& options,
                                                     const logger& log)
        {
            auto sig = read_pe_pdb_signature(pe_path);
            if (!sig)
            {
                throw std::runtime_error("No RSDS PDB reference found in " + pe_path.string());
            }

            std::vector<std::pair<std::filesystem::path, pdb_resolution_source>> direct_candidates{};
            if (sig->path.is_absolute())
            {
                direct_candidates.emplace_back(sig->path, pdb_resolution_source::recorded_path);
            }

            auto sibling_pdb = pe_path;
            sibling_pdb.replace_extension(".pdb");
            direct_candidates.emplace_back(std::move(sibling_pdb), pdb_resolution_source::sibling_path);
            if (sig->path.has_filename())
            {
                direct_candidates.emplace_back(pe_path.parent_path() / sig->path.filename(), pdb_resolution_source::sibling_path);
            }

            for (const auto& [candidate, source] : direct_candidates)
            {
                if (!utils::io::file_exists(candidate))
                {
                    continue;
                }

                const auto parsed = parse_pdb(candidate);
                if (normalize_guid(parsed.signature.guid) != normalize_guid(sig->guid))
                {
                    throw std::runtime_error("PDB signature mismatch for " + candidate.string());
                }

                return resolved_pdb{
                    .path = candidate,
                    .explicit_input = true,
                    .source = source,
                    .source_detail = candidate.parent_path().string(),
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

        bool merge_symbols(mapped_module& mod, const parsed_pdb& pdb)
        {
            bool merged_any = false;
            for (const auto& [address, name] : pdb.symbols)
            {
                const auto rva = section_offset_to_rva(mod, address.first, address.second);
                if (!rva)
                {
                    continue;
                }

                mod.pdb_address_names.try_emplace(mod.image_base + *rva, name);
                merged_any = true;
            }

            return merged_any;
        }

        bool matches_module(const mapped_module& mod, const parsed_pdb& pdb)
        {
            return mod.pdb && normalize_guid(mod.pdb->guid) == normalize_guid(pdb.signature.guid);
        }

        bool should_load_symbols_for_module(const windows_emulator& win_emu, const mapped_module& mod, const pdb_symbol_options& options,
                                            const bool for_callers)
        {
            if (for_callers)
            {
                return options.use_for_callers();
            }

            return options.use_for_trace() &&
                   (options.verbose_logging || win_emu.mod_manager.executable == &mod || options.interesting_modules.contains(mod.name));
        }
    }

    pdb_symbol_loader::pdb_symbol_loader(windows_emulator& win_emu, pdb_symbol_options options)
        : win_emu_(&win_emu),
          options_(std::move(options))
    {
        if (!this->options_.symbol_cache)
        {
            ensure_default_cache_marker();
        }
        prune_sogen_cache(win_emu.log, this->options_.symbol_cache);

        this->module_load_callback_ =
            this->win_emu_->callbacks.on_module_load.add([this](mapped_module& mod) { this->load_for_module(mod, false); });

        this->load_existing_modules();
    }

    pdb_symbol_loader::~pdb_symbol_loader()
    {
        if (this->win_emu_ && this->module_load_callback_)
        {
            this->win_emu_->callbacks.on_module_load.remove(this->module_load_callback_);
        }
    }

    void pdb_symbol_loader::load_existing_modules()
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

    void pdb_symbol_loader::ensure_caller_symbols(const uint64_t address)
    {
        if (!this->options_.use_for_callers())
        {
            return;
        }

        auto* mod = this->win_emu_->mod_manager.executable && this->win_emu_->mod_manager.executable->contains(address)
                        ? this->win_emu_->mod_manager.executable
                        : this->win_emu_->mod_manager.find_by_address(address);
        if (!mod)
        {
            return;
        }

        this->load_for_module(*mod, true);
    }

    void pdb_symbol_loader::load_for_module(mapped_module& mod, const bool for_callers)
    {
        if (mod.pe_sections.empty())
        {
            if (this->options_.verbose_logging)
            {
                this->win_emu_->log.info("Skipping PDB symbols for %s: no PE section metadata\n", mod.name.c_str());
            }
            return;
        }

        if (!should_load_symbols_for_module(*this->win_emu_, mod, this->options_, for_callers))
        {
            return;
        }

        const auto attempt_key = std::to_string(mod.image_base) + (for_callers ? ":callers" : ":trace");
        if (this->attempted_loads_.contains(attempt_key))
        {
            return;
        }
        this->attempted_loads_.insert(attempt_key);

        std::vector<resolved_pdb> candidates{};

        for (const auto& input : this->options_.inputs)
        {
            if (is_pdb_path(input))
            {
                candidates.emplace_back(resolved_pdb{
                    .path = input,
                    .explicit_input = true,
                    .source = pdb_resolution_source::explicit_path,
                    .source_detail = input.parent_path().string(),
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
        else if (this->options_.auto_lookup && this->options_.verbose_logging)
        {
            this->win_emu_->log.info("Skipping automatic PDB lookup for %s: no RSDS reference\n", mod.name.c_str());
        }

        if (candidates.empty() && this->options_.verbose_logging)
        {
            this->win_emu_->log.info("No PDB candidates for %s\n", mod.name.c_str());
        }

        for (const auto& candidate : candidates)
        {
            const auto candidate_key = candidate.path.string() + "@" + std::to_string(mod.image_base);
            if (this->loaded_signatures_.contains(candidate_key))
            {
                continue;
            }

            this->win_emu_->log.info("Loading PDB symbols for %s from %s: %s\n", mod.name.c_str(), source_label(candidate.source),
                                     candidate.path.string().c_str());

            parsed_pdb pdb{};
            try
            {
                pdb = parse_pdb(candidate.path);
            }
            catch (const std::exception& e)
            {
                if (candidate.explicit_input)
                {
                    throw;
                }

                this->win_emu_->log.warn("Failed to parse PDB %s: %s\n", candidate.path.string().c_str(), e.what());
                continue;
            }

            if (!matches_module(mod, pdb))
            {
                const auto same_pdb_name =
                    mod.pdb && utils::string::equals_ignore_case(mod.pdb->path.filename().string(), candidate.path.filename().string());
                if (candidate.explicit_input && same_pdb_name)
                {
                    throw std::runtime_error("PDB signature mismatch for " + candidate.path.string() + " and module " + mod.name);
                }

                if (this->options_.verbose_logging)
                {
                    this->win_emu_->log.info("Skipping PDB %s for %s: signature mismatch\n", candidate.path.string().c_str(),
                                             mod.name.c_str());
                }
                continue;
            }

            if (merge_symbols(mod, pdb))
            {
                mod.has_pdb_symbols = true;
            }
            this->loaded_signatures_.insert(candidate_key);
            this->win_emu_->log.info("Loaded PDB symbols for %s from %s\n", mod.name.c_str(), candidate.path.string().c_str());
        }
    }

} // namespace sogen

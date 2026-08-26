#include "pdb_symbol_source.hpp"

#include <windows_emulator.hpp>
#include <utils/buffer_accessor.hpp>
#include <utils/io.hpp>
#include <utils/string.hpp>
#include <platform/win_pefile.hpp>

#include <cerrno>
#include <cstdlib>
#include <iomanip>
#include <regex>
#include <sstream>

#ifdef _WIN32
#include <Windows.h>
#else
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;
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

        bool pdb_validation_failed(const pdb_validation_status status)
        {
            return status == pdb_validation_status::invalid_artifact || status == pdb_validation_status::validation_error;
        }

        class external_command_exit_error : public std::runtime_error
        {
          public:
            using std::runtime_error::runtime_error;
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

        const std::string& pdbutil_command()
        {
            static const std::string command = [] {
#if defined(__linux__) || defined(__APPLE__)
                std::vector<std::string> names{"llvm-pdbutil"};

#if defined(__APPLE__)
                // Homebrew LLVM is keg-only, so it may not be on PATH.
                names.emplace_back("/opt/homebrew/opt/llvm/bin/llvm-pdbutil"); // Apple Silicon
                names.emplace_back("/usr/local/opt/llvm/bin/llvm-pdbutil");    // Intel
#endif

                for (int version = 14; version <= 30; ++version)
                {
                    names.emplace_back("llvm-pdbutil-" + std::to_string(version));
                }

                for (const auto& name : names)
                {
                    const auto command_line = "command -v " + name;
                    auto* const pipe = popen(command_line.c_str(), "r");
                    if (!pipe)
                    {
                        continue;
                    }

                    std::array<char, 512> buffer{};
                    std::string candidate{};
                    if (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe))
                    {
                        candidate = buffer.data();
                        while (!candidate.empty() && (candidate.back() == '\r' || candidate.back() == '\n'))
                        {
                            candidate.pop_back();
                        }
                    }

                    const auto status = pclose(pipe);
                    if (status == 0 && !candidate.empty())
                    {
                        return candidate;
                    }
                }
#endif

                return std::string{"llvm-pdbutil"};
            }();

            return command;
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

#ifdef _WIN32
        std::string quote_windows_process_arg(const std::string_view arg)
        {
            std::string quoted{};
            quoted.push_back('"');

            size_t backslashes = 0;
            for (const auto ch : arg)
            {
                if (ch == '\\')
                {
                    ++backslashes;
                    continue;
                }

                if (ch == '"')
                {
                    quoted.append(backslashes * 2 + 1, '\\');
                    quoted.push_back(ch);
                    backslashes = 0;
                    continue;
                }

                quoted.append(backslashes, '\\');
                backslashes = 0;
                quoted.push_back(ch);
            }

            quoted.append(backslashes * 2, '\\');
            quoted.push_back('"');
            return quoted;
        }
#endif

        std::string run_command_capture(const std::vector<std::string>& args)
        {
            if (args.empty())
            {
                return {};
            }

#ifdef _WIN32
            SECURITY_ATTRIBUTES security_attributes{};
            security_attributes.nLength = sizeof(security_attributes);
            security_attributes.bInheritHandle = TRUE;

            HANDLE read_pipe{};
            HANDLE write_pipe{};
            if (!CreatePipe(&read_pipe, &write_pipe, &security_attributes, 0))
            {
                throw std::runtime_error("Failed to create output pipe for external command: " + args.front());
            }

            if (!SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0))
            {
                CloseHandle(read_pipe);
                CloseHandle(write_pipe);
                throw std::runtime_error("Failed to configure output pipe for external command: " + args.front());
            }

            std::string command_line{};
            for (const auto& arg : args)
            {
                if (!command_line.empty())
                {
                    command_line.push_back(' ');
                }
                command_line += quote_windows_process_arg(arg);
            }

            STARTUPINFOA startup_info{};
            startup_info.cb = sizeof(startup_info);
            startup_info.dwFlags = STARTF_USESTDHANDLES;
            startup_info.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
            startup_info.hStdOutput = write_pipe;
            startup_info.hStdError = write_pipe;

            PROCESS_INFORMATION process_info{};
            const auto created = CreateProcessA(nullptr, command_line.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr,
                                                &startup_info, &process_info);
            CloseHandle(write_pipe);

            if (!created)
            {
                CloseHandle(read_pipe);
                throw std::runtime_error("Failed to execute external command: " + args.front());
            }

            std::string output{};
            std::array<char, 4096> buffer{};
            DWORD bytes_read{};
            while (ReadFile(read_pipe, buffer.data(), static_cast<DWORD>(buffer.size()), &bytes_read, nullptr) && bytes_read != 0)
            {
                output.append(buffer.data(), bytes_read);
            }

            CloseHandle(read_pipe);
            WaitForSingleObject(process_info.hProcess, INFINITE);

            DWORD exit_code{};
            const auto got_exit_code = GetExitCodeProcess(process_info.hProcess, &exit_code);
            CloseHandle(process_info.hThread);
            CloseHandle(process_info.hProcess);

            if (!got_exit_code)
            {
                throw std::runtime_error("Failed to retrieve external command status: " + args.front());
            }

            if (exit_code != 0)
            {
                throw external_command_exit_error("External command failed: " + args.front() + "\n" + output);
            }

            return output;
#else
            std::vector<char*> argv{};
            argv.reserve(args.size() + 1);
            for (const auto& arg : args)
            {
                argv.push_back(const_cast<char*>(arg.c_str()));
            }
            argv.push_back(nullptr);

            int output_pipe[2]{};
            if (pipe(output_pipe) != 0)
            {
                throw std::runtime_error("Failed to create output pipe for external command: " + args.front());
            }

            posix_spawn_file_actions_t file_actions{};
            if (posix_spawn_file_actions_init(&file_actions) != 0)
            {
                close(output_pipe[0]);
                close(output_pipe[1]);
                throw std::runtime_error("Failed to configure external command: " + args.front());
            }

            auto spawn_error = posix_spawn_file_actions_adddup2(&file_actions, output_pipe[1], STDOUT_FILENO);
            if (spawn_error == 0)
            {
                spawn_error = posix_spawn_file_actions_adddup2(&file_actions, output_pipe[1], STDERR_FILENO);
            }
            if (spawn_error == 0 && output_pipe[0] != STDOUT_FILENO && output_pipe[0] != STDERR_FILENO)
            {
                spawn_error = posix_spawn_file_actions_addclose(&file_actions, output_pipe[0]);
            }
            if (spawn_error == 0 && output_pipe[1] != STDOUT_FILENO && output_pipe[1] != STDERR_FILENO)
            {
                spawn_error = posix_spawn_file_actions_addclose(&file_actions, output_pipe[1]);
            }

            if (spawn_error != 0)
            {
                posix_spawn_file_actions_destroy(&file_actions);
                close(output_pipe[0]);
                close(output_pipe[1]);
                throw std::runtime_error("Failed to configure external command: " + args.front());
            }

            pid_t pid{};
            spawn_error = posix_spawnp(&pid, argv.front(), &file_actions, nullptr, argv.data(), environ);
            posix_spawn_file_actions_destroy(&file_actions);
            close(output_pipe[1]);

            if (spawn_error != 0)
            {
                close(output_pipe[0]);
                throw std::runtime_error("Failed to execute external command: " + args.front());
            }

            std::string output{};
            std::array<char, 4096> buffer{};
            while (true)
            {
                const auto count = read(output_pipe[0], buffer.data(), buffer.size());
                if (count > 0)
                {
                    output.append(buffer.data(), static_cast<size_t>(count));
                    continue;
                }
                if (count == 0)
                {
                    break;
                }
                if (errno == EINTR)
                {
                    continue;
                }

                close(output_pipe[0]);
                int ignored_status{};
                while (waitpid(pid, &ignored_status, 0) < 0 && errno == EINTR)
                {
                }
                throw std::runtime_error("Failed to read external command output: " + args.front());
            }

            close(output_pipe[0]);

            int status{};
            while (waitpid(pid, &status, 0) < 0)
            {
                if (errno == EINTR)
                {
                    continue;
                }
                throw std::runtime_error("Failed to retrieve external command status: " + args.front());
            }

            if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
            {
                throw external_command_exit_error("External command failed: " + args.front() + "\n" + output);
            }

            return output;
#endif
        }

        void parse_pdb_guid_line(const std::string_view trimmed, pdb_signature& signature)
        {
            if (trimmed.starts_with("GUID:"))
            {
                signature.guid = normalize_guid(trim(std::string(trimmed.substr(5))));
            }
        }

        std::optional<uint32_t> read_pdb_dbi_age(const std::filesystem::path& pdb_path)
        {
            const auto output = run_command_capture({pdbutil_command(), "pdb2yaml", "-dbi-stream", pdb_path.string()});

            std::istringstream stream{output};
            std::string line{};
            bool parsing_dbi_stream = false;
            while (std::getline(stream, line))
            {
                const auto trimmed = trim(line);
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

        std::optional<pdb_signature> read_pdb_signature(const std::filesystem::path& pdb_path)
        {
            const auto output = run_command_capture({pdbutil_command(), "dump", "-summary", pdb_path.string()});

            pdb_signature signature{};
            std::istringstream stream{output};
            std::string line{};
            while (std::getline(stream, line))
            {
                parse_pdb_guid_line(trim(line), signature);
            }

            signature.age = read_pdb_dbi_age(pdb_path).value_or(0);
            if (!signature.valid())
            {
                return std::nullopt;
            }

            signature.path = pdb_path.string();
            return signature;
        }

        bool pdb_signatures_match(const pdb_signature& lhs, const pdb_signature& rhs)
        {
            return normalize_guid(lhs.guid) == normalize_guid(rhs.guid) && lhs.age == rhs.age;
        }

        pdb_validation_result validate_pdb_signature(const std::filesystem::path& pdb_path, const pdb_signature& expected)
        {
            try
            {
                const auto actual = read_pdb_signature(pdb_path);
                if (!actual)
                {
                    return {.status = pdb_validation_status::invalid_artifact, .error = "llvm-pdbutil did not report a valid PDB identity"};
                }

                return {.status = pdb_signatures_match(*actual, expected) ? pdb_validation_status::match : pdb_validation_status::mismatch};
            }
            catch (const external_command_exit_error& e)
            {
                return {.status = pdb_validation_status::invalid_artifact, .error = single_line(e.what())};
            }
            catch (const std::exception& e)
            {
                return {.status = pdb_validation_status::validation_error, .error = single_line(e.what())};
            }
        }

        parsed_pdb parse_pdb(const std::filesystem::path& pdb_path)
        {
            const auto output = run_command_capture({pdbutil_command(), "dump", "-summary", "-publics", "-symbols", pdb_path.string()});

            parsed_pdb pdb{};
            std::optional<std::string> pending_public{};
            std::optional<std::string> pending_proc{};
            std::string pending_proc_text{};

            std::istringstream stream{output};
            std::string line{};
            while (std::getline(stream, line))
            {
                const auto trimmed = trim(line);

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

            pdb.signature.age = read_pdb_dbi_age(pdb_path).value_or(0);
            if (!pdb.signature.valid())
            {
                throw std::runtime_error("llvm-pdbutil did not report a valid PDB identity for " + pdb_path.string());
            }

            pdb.signature.path = pdb_path.string();
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
            const std::filesystem::path pdb_name{sig.filename()};
            return root / pdb_name / make_signature_key(sig) / pdb_name;
        }

        std::optional<resolved_pdb> find_in_caches(const pdb_signature& sig, const std::optional<std::filesystem::path>& explicit_cache,
                                                   const logger& log, const std::string_view module_name)
        {
            for (const auto& root : existing_cache_roots(explicit_cache))
            {
                const auto path = symbol_store_path(root, sig);
                if (!utils::io::file_exists(path))
                {
                    continue;
                }

                const auto validation = validate_pdb_signature(path, sig);
                if (validation.status == pdb_validation_status::match)
                {
                    return resolved_pdb{
                        .path = path,
                        .explicit_input = false,
                        .source = pdb_resolution_source::cache,
                        .source_detail = root.string(),
                    };
                }
                if (pdb_validation_failed(validation.status))
                {
                    log.warn("Failed to validate cached PDB for %.*s at %s: %s\n", static_cast<int>(module_name.size()), module_name.data(),
                             path.string().c_str(), validation.error.c_str());
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

            const auto pdb_name = sig.filename();
            result += '/';
            result.append(pdb_name);
            result += '/';
            result += make_signature_key(sig);
            result += '/';
            result.append(pdb_name);
            return result;
        }

        std::filesystem::path temporary_download_path(const std::filesystem::path& target)
        {
            return target.string() + ".tmp";
        }

        download_result download_with_curl(const std::string& url, const std::filesystem::path& target)
        {
            const auto temp = temporary_download_path(target);
            std::error_code ec{};
            std::filesystem::create_directories(target.parent_path(), ec);
            if (ec)
            {
                return {.reason = "failed to create cache directory: " + ec.message()};
            }

            std::filesystem::remove(temp, ec);
            try
            {
                (void)run_command_capture({"curl", "-fsSL", "--connect-timeout", "10", "--max-time", "120", "-o", temp.string(), url});
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

            if (!utils::io::file_exists(temp))
            {
                return {.reason = "download finished but temporary file was not created"};
            }

            return {.success = true};
        }

        download_result promote_download(const std::filesystem::path& temporary_path, const std::filesystem::path& target)
        {
            std::error_code ec{};
            std::filesystem::rename(temporary_path, target, ec);
            if (!ec)
            {
                return {.success = true};
            }

            ec.clear();
            std::filesystem::copy_file(temporary_path, target, std::filesystem::copy_options::overwrite_existing, ec);
            if (ec)
            {
                return {.reason = "failed to promote downloaded PDB into cache: " + ec.message()};
            }

            std::filesystem::remove(temporary_path, ec);
            return {.success = true};
        }

        std::optional<resolved_pdb> find_on_symbol_servers(const pdb_signature& sig, const pdb_symbol_options& options, const logger& log,
                                                           const std::string_view module_name)
        {
            auto download_root = options.symbol_cache.value_or(default_sogen_cache_root());
            const auto download_target = symbol_store_path(download_root, sig);
            if (utils::io::file_exists(download_target))
            {
                const auto validation = validate_pdb_signature(download_target, sig);
                if (validation.status == pdb_validation_status::match)
                {
                    return resolved_pdb{
                        .path = download_target,
                        .explicit_input = false,
                        .source = pdb_resolution_source::cache,
                        .source_detail = download_root.string(),
                    };
                }
                if (pdb_validation_failed(validation.status))
                {
                    log.warn("Failed to validate cached PDB for %.*s at %s: %s\n", static_cast<int>(module_name.size()), module_name.data(),
                             download_target.string().c_str(), validation.error.c_str());
                    return std::nullopt;
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
                        const auto temporary_path = temporary_download_path(download_target);
                        const auto validation = validate_pdb_signature(temporary_path, sig);
                        if (validation.status == pdb_validation_status::match)
                        {
                            const auto promotion = promote_download(temporary_path, download_target);
                            if (promotion.success)
                            {
                                return resolved_pdb{
                                    .path = download_target,
                                    .explicit_input = false,
                                    .source = pdb_resolution_source::download,
                                    .source_detail = server,
                                };
                            }

                            log.warn("Failed to cache downloaded PDB for %.*s from %s: %s\n", static_cast<int>(module_name.size()),
                                     module_name.data(), server.c_str(), promotion.reason.c_str());
                            std::error_code ec{};
                            std::filesystem::remove(temporary_path, ec);
                            continue;
                        }
                        if (validation.status == pdb_validation_status::validation_error)
                        {
                            log.warn("Failed to validate downloaded PDB for %.*s from %s: %s\n", static_cast<int>(module_name.size()),
                                     module_name.data(), server.c_str(), validation.error.c_str());
                            std::error_code ec{};
                            std::filesystem::remove(temporary_path, ec);
                            return std::nullopt;
                        }

                        if (validation.status == pdb_validation_status::mismatch)
                        {
                            log.warn("Downloaded PDB for %.*s from %s did not match expected GUID and age\n",
                                     static_cast<int>(module_name.size()), module_name.data(), server.c_str());
                        }
                        else
                        {
                            log.warn("Downloaded symbol artifact for %.*s from %s was not a usable PDB: %s\n",
                                     static_cast<int>(module_name.size()), module_name.data(), server.c_str(), validation.error.c_str());
                        }
                        std::error_code ec{};
                        std::filesystem::remove(temporary_path, ec);
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
                if (!utils::io::file_exists(path))
                {
                    continue;
                }

                const auto validation = validate_pdb_signature(path, sig);
                if (validation.status == pdb_validation_status::match)
                {
                    return resolved_pdb{
                        .path = path,
                        .explicit_input = false,
                        .source = pdb_resolution_source::symbol_store,
                        .source_detail = server,
                    };
                }
                if (pdb_validation_failed(validation.status))
                {
                    log.warn("Failed to validate PDB for %.*s in symbol store %s: %s\n", static_cast<int>(module_name.size()),
                             module_name.data(), server.c_str(), validation.error.c_str());
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

            const std::filesystem::path recorded_path{sig.path};
            if (recorded_path.is_absolute() && utils::io::file_exists(recorded_path))
            {
                const auto validation = validate_pdb_signature(recorded_path, sig);
                if (validation.status == pdb_validation_status::match)
                {
                    return resolved_pdb{
                        .path = recorded_path,
                        .explicit_input = false,
                        .source = pdb_resolution_source::recorded_path,
                        .source_detail = recorded_path.parent_path().string(),
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

                    const auto validation = validate_pdb_signature(candidate, sig);
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
                        .source_detail = candidate.parent_path().string(),
                    };
                }
            }

            if (const auto cached = find_in_caches(sig, options.symbol_cache, log, module_name))
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

                const auto parsed = parse_pdb(candidate);
                if (!pdb_signatures_match(parsed.signature, *sig))
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

        void merge_symbols(loaded_symbols& result, const mapped_module& mod, const parsed_pdb& pdb)
        {
            for (const auto& [address, name] : pdb.symbols)
            {
                if (const auto rva = symbol_address_to_rva(mod, address.first, address.second))
                {
                    result.address_names.try_emplace(*rva, name);
                }
            }
        }

        bool matches_module(const mapped_module& mod, const parsed_pdb& pdb)
        {
            return mod.pdb && pdb_signatures_match(*mod.pdb, pdb.signature);
        }

    }

    pdb_symbol_source::pdb_symbol_source(windows_emulator& win_emu, pdb_symbol_options options)
        : win_emu_(&win_emu),
          options_(std::move(options))
    {
        if (!this->options_.symbol_cache)
        {
            ensure_default_cache_marker();
        }
        prune_sogen_cache(win_emu.log, this->options_.symbol_cache);
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

        std::set<std::filesystem::path> loaded_candidates{};
        for (const auto& candidate : candidates)
        {
            if (!loaded_candidates.insert(candidate.path).second)
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
                const auto candidate_filename = candidate.path.filename().string();
                const auto same_pdb_name =
                    mod.pdb && utils::string::equals_ignore_case(mod.pdb->filename(), std::string_view{candidate_filename});
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

            merge_symbols(result, mod, pdb);
            this->win_emu_->log.info("Loaded PDB symbols for %s from %s\n", mod.name.c_str(), candidate.path.string().c_str());
        }

        return result;
    }

} // namespace sogen

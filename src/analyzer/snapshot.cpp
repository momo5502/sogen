#include "snapshot.hpp"

#include <utils/io.hpp>
#include <utils/compression.hpp>

namespace snapshot
{
    namespace
    {
        struct snapshot_header
        {
            // NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays,hicpp-avoid-c-arrays,modernize-avoid-c-arrays)
            char magic[4] = {'S', 'N', 'A', 'P'};
            uint32_t version{1};
        };

        static_assert(sizeof(snapshot_header) == 8);

        std::span<const std::byte> validate_header(const std::span<const std::byte> snapshot)
        {
            snapshot_header header{};
            constexpr snapshot_header default_header{};

            if (snapshot.size() < sizeof(header))
            {
                throw std::runtime_error("Snapshot is too small");
            }

            memcpy(&header, snapshot.data(), sizeof(header));

            if (memcmp(default_header.magic, header.magic, sizeof(header.magic)) != 0)
            {
                throw std::runtime_error("Invalid snapshot");
            }

            if (default_header.version != header.version)
            {
                throw std::runtime_error("Unsupported snapshot version: " + std::to_string(header.version) +
                                         "(needed: " + std::to_string(default_header.version) + ")");
            }

            return snapshot.subspan(sizeof(header));
        }

        std::vector<std::byte> get_compressed_emulator_state(const windows_emulator& win_emu)
        {
            utils::buffer_serializer serializer{};
            win_emu.serialize(serializer);

            return utils::compression::zlib::compress(serializer.get_buffer());
        }

        std::vector<std::byte> get_decompressed_emulator_state(const std::span<const std::byte> snapshot)
        {
            const auto data = validate_header(snapshot);
            return utils::compression::zlib::decompress(data);
        }

        std::string get_main_executable_name(const windows_emulator& win_emu)
        {
            const auto* exe = win_emu.mod_manager.executable;
            if (exe)
            {
                return std::filesystem::path(exe->name).stem().string();
            }

            return "process";
        }
    }

    std::vector<std::byte> create_emulator_snapshot(const windows_emulator& win_emu)
    {
        const auto state = get_compressed_emulator_state(win_emu);

        snapshot_header header{};
        std::span header_span(reinterpret_cast<const std::byte*>(&header), sizeof(header));

        std::vector<std::byte> snapshot{};
        snapshot.reserve(header_span.size() + state.size());
        snapshot.assign(header_span.begin(), header_span.end());
        snapshot.insert(snapshot.end(), state.begin(), state.end());

        return snapshot;
    }

    std::filesystem::path write_emulator_snapshot(const windows_emulator& win_emu, const bool log)
    {
        std::filesystem::path snapshot_file = get_main_executable_name(win_emu) + "-" + std::to_string(time(nullptr)) + ".snap";

        if (log)
        {
            win_emu.log.log("Writing snapshot to %s...\n", snapshot_file.string().c_str());
        }

        const auto snapshot = create_emulator_snapshot(win_emu);
        if (!utils::io::write_file(snapshot_file, snapshot))
        {
            throw std::runtime_error("Failed to write snapshot!");
        }

        return snapshot_file;
    }

    void load_emulator_snapshot(windows_emulator& win_emu, const std::span<const std::byte> snapshot)
    {
        const auto data = get_decompressed_emulator_state(snapshot);

        utils::buffer_deserializer deserializer{data};
        win_emu.deserialize(deserializer);
    }

    void load_emulator_snapshot(windows_emulator& win_emu, const std::filesystem::path& snapshot_file)
    {
        std::vector<std::byte> data{};
        if (!utils::io::read_file(snapshot_file, &data))
        {
            throw std::runtime_error("Failed to read snapshot file: " + snapshot_file.string());
        }

        load_emulator_snapshot(win_emu, data);
    }
}

#include "std_include.hpp"

#include "snapshot.hpp"

#include <utils/io.hpp>
#include <utils/finally.hpp>
#include <utils/compression.hpp>

#include <algorithm>
#include <array>
#include <fstream>
#include <random>
#include <sstream>

namespace sogen
{

    namespace snapshot
    {
        namespace
        {
            struct snapshot_header
            {
                // NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays,hicpp-avoid-c-arrays,modernize-avoid-c-arrays)
                char magic[4] = {'S', 'N', 'A', 'P'};
                uint32_t version{7};
                uint32_t machine{static_cast<uint32_t>(PEMachineType::AMD64)};
                uint32_t reserved{};
                // A streamed zstd frame does not record its decompressed size.
                uint64_t state_size{};
            };

            static_assert(sizeof(snapshot_header) == 24);

            snapshot_header read_header(const std::span<const std::byte> snapshot)
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

                if (header.reserved != 0)
                {
                    throw std::runtime_error("Invalid snapshot");
                }

                return header;
            }

            std::vector<std::byte> get_decompressed_emulator_state(const std::span<const std::byte> snapshot)
            {
                const auto header = read_header(snapshot);
                auto state = utils::compression::zstd::decompress(snapshot.subspan(sizeof(snapshot_header)), header.state_size);
                if (state.size() != header.state_size)
                {
                    throw std::runtime_error("Failed to decompress snapshot");
                }

                return state;
            }

            snapshot_header make_header(const windows_emulator& win_emu, const size_t state_size)
            {
                snapshot_header header{};
                header.machine = static_cast<uint32_t>(
                    win_emu.process.native_architecture == windows_guest_architecture::arm64 ? PEMachineType::ARM64 : PEMachineType::AMD64);
                header.state_size = state_size;
                return header;
            }

            std::span<const std::byte> header_bytes(const snapshot_header& header)
            {
                return {reinterpret_cast<const std::byte*>(&header), sizeof(header)};
            }

            void serialize_emulator_state(const windows_emulator& win_emu, const std::string_view configuration,
                                          utils::buffer_serializer& serializer)
            {
                serializer.write_string(configuration);
                win_emu.serialize(serializer);
            }

            void deserialize_emulator_state(windows_emulator& win_emu, const std::string_view configuration,
                                            utils::buffer_deserializer& deserializer)
            {
                if (deserializer.read_string<char>() != configuration)
                {
                    throw std::runtime_error("The snapshot was taken with a different emulator configuration");
                }

                win_emu.deserialize(deserializer);
            }

            // Every worker keeps its own buffers, which a device with little memory may not afford while saving a large process.
            constexpr unsigned max_compression_workers = 4;

            std::string random_suffix()
            {
                std::random_device device{};
                return (std::stringstream{} << std::hex << device() << device()).str();
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

        std::vector<std::byte> create_emulator_snapshot(windows_emulator& win_emu, const std::string_view configuration)
        {
            win_emu.emu().ensure_restorable_in_new_process();
            win_emu.prepare_serialization();

            utils::buffer_serializer serializer{};
            serialize_emulator_state(win_emu, configuration, serializer);
            const auto state = utils::compression::zstd::compress(serializer.get_buffer(), 1);

            const auto header = make_header(win_emu, serializer.size());
            const auto header_span = header_bytes(header);

            std::vector<std::byte> snapshot{};
            snapshot.reserve(header_span.size() + state.size());
            snapshot.assign(header_span.begin(), header_span.end());
            snapshot.insert(snapshot.end(), state.begin(), state.end());

            return snapshot;
        }

        std::filesystem::path write_emulator_snapshot(windows_emulator& win_emu, const bool log)
        {
            std::filesystem::path snapshot_file = get_main_executable_name(win_emu) + "-" + std::to_string(time(nullptr)) + ".snap";

            if (log)
            {
                win_emu.log.log("Writing snapshot to %s...\n", snapshot_file.string().c_str());
            }

            write_emulator_snapshot(win_emu, snapshot_file);
            return snapshot_file;
        }

        void write_emulator_snapshot(windows_emulator& win_emu, const std::filesystem::path& snapshot_file,
                                     const std::string_view configuration, const std::function<bool()>& should_commit)
        {
            win_emu.emu().ensure_restorable_in_new_process();
            win_emu.prepare_serialization();

            // Written next to the destination and moved over it, so a failed write never leaves a truncated snapshot behind.
            auto temporary_file = snapshot_file;
            temporary_file += "." + random_suffix() + ".tmp";

            auto remove_temporary_file = utils::finally([&] {
                std::error_code ec{};
                std::filesystem::remove(temporary_file, ec);
            });

            {
                std::ofstream stream{temporary_file, std::ios::binary | std::ios::trunc};
                const auto write = [&](const std::span<const std::byte> data) {
                    stream.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
                };

                // The state is streamed to the file rather than held in memory, so its size is filled into the header last.
                write(header_bytes(make_header(win_emu, 0)));

                utils::compression::zstd::stream_compressor compressor{
                    write, 1, std::min(std::thread::hardware_concurrency(), max_compression_workers)};
                utils::buffer_serializer serializer{[&](const std::span<const std::byte> data) { compressor.write(data); }};
                serialize_emulator_state(win_emu, configuration, serializer);
                serializer.flush();
                compressor.finish();

                stream.seekp(0);
                write(header_bytes(make_header(win_emu, serializer.size())));

                stream.close();
                if (stream.fail())
                {
                    throw std::runtime_error("Failed to write snapshot: " + snapshot_file.string());
                }
            }

            if (should_commit && !should_commit())
            {
                throw std::runtime_error("The process was stopped while it was saved");
            }

            std::filesystem::rename(temporary_file, snapshot_file);
            remove_temporary_file.cancel();
        }

        PEMachineType get_snapshot_machine(const std::filesystem::path& snapshot_file)
        {
            std::array<std::byte, sizeof(snapshot_header)> header{};
            std::ifstream stream{snapshot_file, std::ios::binary};
            if (!stream.read(reinterpret_cast<char*>(header.data()), header.size()))
            {
                throw std::runtime_error("Failed to read snapshot file: " + snapshot_file.string());
            }

            return static_cast<PEMachineType>(read_header(header).machine);
        }

        void load_emulator_snapshot(windows_emulator& win_emu, const std::span<const std::byte> snapshot,
                                    const std::string_view configuration)
        {
            const auto data = get_decompressed_emulator_state(snapshot);

            utils::buffer_deserializer deserializer{data};
            deserialize_emulator_state(win_emu, configuration, deserializer);
        }

        void load_emulator_snapshot(windows_emulator& win_emu, const std::filesystem::path& snapshot_file,
                                    const std::string_view configuration)
        {
            std::ifstream stream{snapshot_file, std::ios::binary};
            std::array<std::byte, sizeof(snapshot_header)> header_data{};
            if (!stream.read(reinterpret_cast<char*>(header_data.data()), header_data.size()))
            {
                throw std::runtime_error("Failed to read snapshot file: " + snapshot_file.string());
            }

            // The state is decompressed while it is restored rather than held in memory.
            utils::compression::zstd::stream_decompressor decompressor{[&](const std::span<std::byte> compressed) {
                stream.read(reinterpret_cast<char*>(compressed.data()), static_cast<std::streamsize>(compressed.size()));
                return static_cast<size_t>(stream.gcount());
            }};

            utils::buffer_deserializer deserializer{[&](const std::span<std::byte> data) { decompressor.read(data); },
                                                    static_cast<size_t>(read_header(header_data).state_size)};
            deserialize_emulator_state(win_emu, configuration, deserializer);

            if (deserializer.get_remaining_size() != 0)
            {
                throw std::runtime_error("The snapshot holds more state than was restored");
            }

            decompressor.finish();
        }

        controller::controller(windows_emulator& win_emu, std::string configuration, std::filesystem::path directory,
                               outcome_callback on_outcome)
            : win_emu_(win_emu),
              configuration_(std::move(configuration)),
              directory_(std::move(directory)),
              on_outcome_(std::move(on_outcome)),
              watcher_([this] { this->watch(); })
        {
        }

        controller::~controller()
        {
            {
                const std::scoped_lock lock(this->mutex_);
                this->watching_ = false;
            }
            this->changed_.notify_all();
            this->watcher_.join();
        }

        bool controller::request(std::filesystem::path destination)
        {
            {
                const std::scoped_lock lock(this->mutex_);
                if (this->pending_)
                {
                    return false;
                }
                this->pending_ = std::move(destination);
            }
            this->changed_.notify_all();
            return true;
        }

        void controller::stop()
        {
            const std::scoped_lock lock(this->mutex_);
            this->stop_requested_ = true;
            this->win_emu_.stop();
        }

        // Requesting a suspend takes locks the emulator may hold while it handles input, so it happens on this thread. Starting
        // the emulator clears a stop that came just before, so stops and requests are repeated until the run ends.
        void controller::watch()
        {
            std::unique_lock lock(this->mutex_);
            while (this->watching_)
            {
                this->changed_.wait_for(lock, std::chrono::milliseconds(50));
                if (!this->running_)
                {
                    continue;
                }

                if (this->stop_requested_)
                {
                    this->win_emu_.stop();
                    continue;
                }

                if (!this->pending_)
                {
                    continue;
                }

                const auto now = std::chrono::steady_clock::now();
                if (!this->suspend_requested_)
                {
                    this->suspend_requested_ = true;
                    this->deadline_ = now + std::chrono::seconds(2);
                }

                if (now >= this->deadline_)
                {
                    this->win_emu_.stop();
                }
                else
                {
                    this->win_emu_.request_suspend();
                }
            }
        }

        std::filesystem::path controller::next_snapshot_path() const
        {
            return this->directory_ /
                   (get_main_executable_name(this->win_emu_) + "-" + std::to_string(time(nullptr)) + "-" + random_suffix() + ".snap");
        }

        bool controller::run()
        {
            while (true)
            {
                {
                    const std::scoped_lock lock(this->mutex_);
                    if (this->stop_requested_)
                    {
                        return false;
                    }
                    this->running_ = true;
                }

                this->win_emu_.start();

                std::unique_lock lock(this->mutex_);
                this->running_ = false;
                if (!this->pending_)
                {
                    return false;
                }

                snapshot_outcome outcome{.path = this->pending_->empty() ? this->next_snapshot_path() : *this->pending_};
                bool ended = this->stop_requested_ || this->win_emu_.process.exit_status.has_value();
                lock.unlock();

                if (ended)
                {
                    outcome.error = "The process ended before it was saved";
                }
                else
                {
                    try
                    {
                        if (outcome.path.has_parent_path())
                        {
                            std::filesystem::create_directories(outcome.path.parent_path());
                        }
                        write_emulator_snapshot(this->win_emu_, outcome.path, this->configuration_, [this] {
                            const std::scoped_lock commit_lock(this->mutex_);
                            return !this->stop_requested_;
                        });
                    }
                    catch (const std::exception& e)
                    {
                        outcome.error = e.what();
                    }

                    if (!outcome.error.empty())
                    {
                        lock.lock();
                        ended = this->stop_requested_;
                        lock.unlock();
                    }
                }

                lock.lock();
                this->pending_.reset();
                this->suspend_requested_ = false;
                this->deadline_ = std::chrono::steady_clock::time_point::max();
                this->win_emu_.cancel_suspend_request();
                lock.unlock();

                if (this->on_outcome_)
                {
                    this->on_outcome_(outcome);
                }

                if (ended || outcome.error.empty())
                {
                    return outcome.error.empty();
                }
            }
        }
    }

} // namespace sogen

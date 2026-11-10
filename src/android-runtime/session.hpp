#pragma once

#include "android_ui_backend.hpp"
#include "java_dispatcher.hpp"
#include "pseudo_terminal.hpp"

#include <frame_pacer.hpp>
#include <display_modes.hpp>
#include <filesystem/directory_mount.hpp>
#include <filesystem/document_broker.hpp>

#include <jni.h>

#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace sogen
{
    class windows_emulator;

    namespace snapshot
    {
        class controller;
        struct snapshot_outcome;
    }
}

namespace sogen
{
    class subprocess_process_manager;
}

namespace sogen::android
{
    class android_process_launcher;

    // A Storage Access Framework tree, listed by the app before the session starts, mounted copy-on-write.
    struct document_tree
    {
        std::string guest_path{};
        std::string uri{};
        std::filesystem::path upper_directory{};
        std::filesystem::path state_directory{};
        std::vector<document_entry> entries{};
    };

    struct run_config
    {
        std::filesystem::path emulation_root{};
        std::string application{};
        std::vector<std::string> arguments{};
        std::vector<std::pair<std::string, std::string>> environment{};
        // Where the game's changes to the emulation root are kept. The root itself is never written.
        std::filesystem::path root_layer{};
        std::vector<directory_mount> directory_mounts{};
        std::vector<document_tree> document_trees{};
        std::string working_directory{};
        std::string backend{};
        bool live_network{};
        std::optional<display_mode> max_display_mode{};
        uint32_t steam_app_id{};
        uint64_t steam_id{};
        std::string steam_language{};
        std::vector<uint8_t> steam_ownership_ticket{};
        std::string steam_ownership_ticket_digest{};
        std::filesystem::path sandbox_executable{};
        vulkan_driver vulkan{};
        // Where a suspend without a destination saves the process.
        std::filesystem::path snapshot_directory{};
        // A saved process to continue instead of starting the application.
        std::filesystem::path resume_file{};
        // Registry values imported into a fresh session over the saved registry state.
        std::vector<std::filesystem::path> registry_files{};
        // Like registry_files, but their strings refer to the guest's environment variables, such as %USERPROFILE%.
        std::vector<std::filesystem::path> expanded_registry_files{};
        // Where the guest's registry changes are kept between sessions; empty keeps none.
        std::filesystem::path registry_state{};
        // What the parent's process manager passed to this child process; empty for the game itself.
        std::vector<std::string> managed_arguments{};
    };

    class session final : public ui_host
    {
      public:
        session(JavaVM& vm, JNIEnv& env, jobject listener);
        ~session() override;

        session(const session&) = delete;
        session& operator=(const session&) = delete;
        session(session&&) = delete;
        session& operator=(session&&) = delete;

        int run(const run_config& config);
        void stop();

        // Saves the process and ends the session. Returns false while it is not running or already being saved.
        bool suspend(std::filesystem::path destination);

        // 0 turns the limit off. Applies to the running emulator and to one started later.
        void set_frame_limit(uint32_t frames_per_second);

        // Empty while no emulator runs.
        std::optional<frame_pacer::statistics> frame_statistics();

        void with_ui(const std::function<void(android_ui_backend&)>& action);

        void console_input(std::string_view data) const;
        void console_resize(uint16_t columns, uint16_t rows) const;

        // The app's answers to a child process this session asked it to start.
        void child_started(uint64_t request, int32_t process_id);
        void child_exited(uint64_t request, int32_t exit_status);

        void on_window_created(hwnd window, int32_t width, int32_t height, std::u16string_view title, bool visible) override;
        void on_window_destroyed(hwnd window) override;
        void on_window_resized(hwnd window, int32_t width, int32_t height) override;
        void on_window_visibility_changed(hwnd window, bool visible) override;
        void on_window_title_changed(hwnd window, std::u16string_view title) override;
        void on_cursor_moved(hwnd window, int32_t x, int32_t y) override;
        void on_relative_mouse_changed(hwnd window, bool relative) override;

      private:
        int run_emulator(const run_config& config);
        void report_snapshot(const snapshot::snapshot_outcome& outcome);
        void report_guest_exit();
        unique_fd open_document(const std::string& tree, const std::string& document_id);
        void start_child(uint64_t request, const std::vector<std::string>& arguments);
        void kill_child(uint64_t request);

        struct listener_methods
        {
            jmethodID on_output{};
            jmethodID on_console_output{};
            jmethodID on_window_created{};
            jmethodID on_window_destroyed{};
            jmethodID on_window_resized{};
            jmethodID on_window_visibility_changed{};
            jmethodID on_window_title_changed{};
            jmethodID on_cursor_moved{};
            jmethodID on_relative_mouse_changed{};
            jmethodID on_snapshot{};
            jmethodID on_guest_exited{};
            jmethodID open_document{};
            jmethodID start_child{};
            jmethodID kill_child{};
        };

        jobject listener_{};
        listener_methods methods_{};
        java_dispatcher dispatcher_;

        std::mutex mutex_{};
        windows_emulator* emulator_{};
        snapshot::controller* snapshots_{};
        android_ui_backend* ui_{};
        android_process_launcher* launcher_{};
        subprocess_process_manager* processes_{};
        bool stop_requested_{};
        uint32_t frame_limit_{};
        // The Steam provider's interfaces live outside the saved process, so a Steam game cannot be suspended yet.
        bool runs_steam_game_{};

        std::optional<pseudo_terminal> terminal_{};

        void output(std::string text, int32_t color);
    };
}

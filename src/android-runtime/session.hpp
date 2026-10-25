#pragma once

#include "android_ui_backend.hpp"
#include "java_dispatcher.hpp"
#include "pseudo_terminal.hpp"

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

namespace sogen::android
{
    struct run_config
    {
        std::filesystem::path emulation_root{};
        std::string application{};
        std::vector<std::string> arguments{};
        std::vector<std::pair<std::string, std::string>> environment{};
        std::vector<std::pair<std::string, std::filesystem::path>> path_mappings{};
        std::string working_directory{};
        std::string backend{};
        bool live_network{};
        std::filesystem::path sandbox_executable{};
        // Where a suspend without a destination saves the process.
        std::filesystem::path snapshot_directory{};
        // A saved process to continue instead of starting the application.
        std::filesystem::path resume_file{};
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

        void with_ui(const std::function<void(android_ui_backend&)>& action);

        void console_input(std::string_view data) const;
        void console_resize(uint16_t columns, uint16_t rows) const;

        void on_window_created(hwnd window, int32_t width, int32_t height, std::u16string_view title, bool visible) override;
        void on_window_destroyed(hwnd window) override;
        void on_window_resized(hwnd window, int32_t width, int32_t height) override;
        void on_window_visibility_changed(hwnd window, bool visible) override;
        void on_window_title_changed(hwnd window, std::u16string_view title) override;
        void on_cursor_moved(hwnd window, int32_t x, int32_t y) override;

      private:
        int run_emulator(const run_config& config);
        void report_snapshot(const snapshot::snapshot_outcome& outcome);

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
            jmethodID on_snapshot{};
        };

        jobject listener_{};
        listener_methods methods_{};
        java_dispatcher dispatcher_;

        std::mutex mutex_{};
        windows_emulator* emulator_{};
        snapshot::controller* snapshots_{};
        android_ui_backend* ui_{};
        bool stop_requested_{};

        std::optional<pseudo_terminal> terminal_{};

        void output(std::string text, int32_t color);
    };
}

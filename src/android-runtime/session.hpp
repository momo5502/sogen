#pragma once

#include "android_ui_backend.hpp"
#include "java_dispatcher.hpp"

#include <jni.h>

#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace sogen
{
    class windows_emulator;
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

        void with_ui(const std::function<void(android_ui_backend&)>& action);

        void on_window_created(hwnd window, int32_t width, int32_t height, std::u16string_view title, bool visible) override;
        void on_window_destroyed(hwnd window) override;
        void on_window_resized(hwnd window, int32_t width, int32_t height) override;
        void on_window_visibility_changed(hwnd window, bool visible) override;
        void on_window_title_changed(hwnd window, std::u16string_view title) override;

      private:
        int run_emulator(const run_config& config);

        struct listener_methods
        {
            jmethodID on_output{};
            jmethodID on_window_created{};
            jmethodID on_window_destroyed{};
            jmethodID on_window_resized{};
            jmethodID on_window_visibility_changed{};
            jmethodID on_window_title_changed{};
        };

        jobject listener_{};
        listener_methods methods_{};
        java_dispatcher dispatcher_;

        std::mutex mutex_{};
        windows_emulator* emulator_{};
        android_ui_backend* ui_{};
        bool stop_requested_{};

        // [color] is a logger color, or -1 for the guest's own stdout.
        void output(std::string text, int32_t color);
    };
}

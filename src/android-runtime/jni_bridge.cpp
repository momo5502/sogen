#include "std_include.hpp"
#include "session.hpp"

#include <android/native_window_jni.h>

#include <algorithm>
#include <array>

namespace
{
    using sogen::android::session;

    session& get_session(const jlong handle)
    {
        return *reinterpret_cast<session*>(handle);
    }

    std::u16string to_u16string(JNIEnv& env, const jstring value)
    {
        if (!value)
        {
            return {};
        }

        const auto length = env.GetStringLength(value);
        std::u16string result(static_cast<size_t>(length), u'\0');
        env.GetStringRegion(value, 0, length, reinterpret_cast<jchar*>(result.data()));
        return result;
    }

    std::string to_string(JNIEnv& env, const jstring value)
    {
        return sogen::u16_to_u8(to_u16string(env, value));
    }

    std::vector<std::string> to_strings(JNIEnv& env, const jobjectArray values)
    {
        std::vector<std::string> result{};
        if (!values)
        {
            return result;
        }

        const auto count = env.GetArrayLength(values);
        result.reserve(static_cast<size_t>(count));

        for (jsize i = 0; i < count; ++i)
        {
            auto* element = static_cast<jstring>(env.GetObjectArrayElement(values, i));
            result.push_back(to_string(env, element));
            env.DeleteLocalRef(element);
        }

        return result;
    }

    template <typename Second>
    std::vector<std::pair<std::string, Second>> to_pairs(JNIEnv& env, const jobjectArray values)
    {
        const auto flat = to_strings(env, values);

        std::vector<std::pair<std::string, Second>> result{};
        for (size_t i = 0; i + 1 < flat.size(); i += 2)
        {
            result.emplace_back(flat[i], Second{flat[i + 1]});
        }

        return result;
    }

    template <typename Array, typename Element>
    std::vector<Element> to_vector(JNIEnv& env, const Array values, void (JNIEnv::*get_region)(Array, jsize, jsize, Element*))
    {
        std::vector<Element> result(values ? static_cast<size_t>(env.GetArrayLength(values)) : 0);
        if (!result.empty())
        {
            (env.*get_region)(values, 0, static_cast<jsize>(result.size()), result.data());
        }

        return result;
    }

    jobject get_object_field(JNIEnv& env, const jobject object, const char* name, const char* signature)
    {
        auto* object_class = env.GetObjectClass(object);
        auto* value = env.GetObjectField(object, env.GetFieldID(object_class, name, signature));
        env.DeleteLocalRef(object_class);
        return value;
    }

    std::string get_string_field(JNIEnv& env, const jobject object, const char* name)
    {
        auto* value = static_cast<jstring>(get_object_field(env, object, name, "Ljava/lang/String;"));
        auto result = to_string(env, value);
        env.DeleteLocalRef(value);
        return result;
    }

    sogen::android::document_tree to_document_tree(JNIEnv& env, const jobject tree)
    {
        auto* path_array = static_cast<jobjectArray>(get_object_field(env, tree, "paths", "[Ljava/lang/String;"));
        auto* id_array = static_cast<jobjectArray>(get_object_field(env, tree, "ids", "[Ljava/lang/String;"));
        auto* size_array = static_cast<jlongArray>(get_object_field(env, tree, "sizes", "[J"));
        auto* modified_array = static_cast<jlongArray>(get_object_field(env, tree, "modified", "[J"));
        auto* directory_array = static_cast<jbooleanArray>(get_object_field(env, tree, "directories", "[Z"));

        const auto paths = to_strings(env, path_array);
        const auto ids = to_strings(env, id_array);
        const auto sizes = to_vector(env, size_array, &JNIEnv::GetLongArrayRegion);
        const auto modified = to_vector(env, modified_array, &JNIEnv::GetLongArrayRegion);
        const auto directories = to_vector(env, directory_array, &JNIEnv::GetBooleanArrayRegion);

        env.DeleteLocalRef(path_array);
        env.DeleteLocalRef(id_array);
        env.DeleteLocalRef(size_array);
        env.DeleteLocalRef(modified_array);
        env.DeleteLocalRef(directory_array);

        const auto count = paths.size();
        if (ids.size() != count || sizes.size() != count || modified.size() != count || directories.size() != count)
        {
            throw std::runtime_error("Inconsistent document tree listing");
        }

        sogen::android::document_tree result{
            .guest_path = get_string_field(env, tree, "guestPath"),
            .uri = get_string_field(env, tree, "uri"),
            .upper_directory = get_string_field(env, tree, "upperDirectory"),
            .state_directory = get_string_field(env, tree, "stateDirectory"),
        };

        result.entries.reserve(count);
        for (size_t i = 0; i < count; ++i)
        {
            result.entries.push_back(sogen::document_entry{
                .path = sogen::u8_to_u16(paths[i]),
                .id = ids[i],
                .is_directory = directories[i] == JNI_TRUE,
                .size = static_cast<uint64_t>(sizes[i]),
                .modified_ms = modified[i],
            });
        }

        return result;
    }

    std::vector<sogen::android::document_tree> to_document_trees(JNIEnv& env, const jobjectArray trees)
    {
        std::vector<sogen::android::document_tree> result{};
        const auto count = trees ? env.GetArrayLength(trees) : 0;
        for (jsize i = 0; i < count; ++i)
        {
            auto* tree = env.GetObjectArrayElement(trees, i);
            result.push_back(to_document_tree(env, tree));
            env.DeleteLocalRef(tree);
        }

        return result;
    }

    sogen::android::mouse_button to_mouse_button(const jint button)
    {
        switch (button)
        {
        case 1:
            return sogen::android::mouse_button::right;
        case 2:
            return sogen::android::mouse_button::middle;
        default:
            return sogen::android::mouse_button::left;
        }
    }
}

extern "C"
{
    JNIEXPORT jlong JNICALL Java_dev_sogen_app_runtime_SogenNative_nativeCreate(JNIEnv* env, jobject /*self*/, jobject listener)
    {
        JavaVM* vm{};
        if (env->GetJavaVM(&vm) != JNI_OK)
        {
            return 0;
        }

        return reinterpret_cast<jlong>(new session(*vm, *env, listener));
    }

    JNIEXPORT void JNICALL Java_dev_sogen_app_runtime_SogenNative_nativeDestroy(JNIEnv* /*env*/, jobject /*self*/, const jlong handle)
    {
        delete reinterpret_cast<session*>(handle);
    }

    JNIEXPORT jint JNICALL Java_dev_sogen_app_runtime_SogenNative_nativeRun(
        JNIEnv* env, jobject /*self*/, const jlong handle, jstring root, jstring application, jobjectArray arguments,
        jobjectArray environment, jobjectArray path_mappings, jobjectArray document_trees, jstring working_directory, jstring backend,
        const jboolean live_network, jstring sandbox_executable, jstring vulkan_hook_directory, jstring vulkan_driver_directory,
        jstring vulkan_driver_name, jstring snapshot_directory, jstring resume_file)
    {
        sogen::android::run_config config{
            .emulation_root = to_string(*env, root),
            .application = to_string(*env, application),
            .arguments = to_strings(*env, arguments),
            .environment = to_pairs<std::string>(*env, environment),
            .path_mappings = to_pairs<std::filesystem::path>(*env, path_mappings),
            .working_directory = to_string(*env, working_directory),
            .backend = to_string(*env, backend),
            .live_network = live_network == JNI_TRUE,
            .sandbox_executable = to_string(*env, sandbox_executable),
            .vulkan =
                {
                    .hook_directory = to_string(*env, vulkan_hook_directory),
                    .custom_directory = to_string(*env, vulkan_driver_directory),
                    .custom_name = to_string(*env, vulkan_driver_name),
                },
            .snapshot_directory = to_string(*env, snapshot_directory),
            .resume_file = to_string(*env, resume_file),
        };

        try
        {
            config.document_trees = to_document_trees(*env, document_trees);
        }
        catch (const std::exception& e)
        {
            env->ThrowNew(env->FindClass("java/lang/IllegalArgumentException"), e.what());
            return 1;
        }

        return get_session(handle).run(config);
    }

    JNIEXPORT void JNICALL Java_dev_sogen_app_runtime_SogenNative_nativeStop(JNIEnv* /*env*/, jobject /*self*/, const jlong handle)
    {
        get_session(handle).stop();
    }

    JNIEXPORT jboolean JNICALL Java_dev_sogen_app_runtime_SogenNative_nativeSuspend(JNIEnv* env, jobject /*self*/, const jlong handle,
                                                                                    jstring destination)
    {
        return get_session(handle).suspend(to_string(*env, destination)) ? JNI_TRUE : JNI_FALSE;
    }

    JNIEXPORT void JNICALL Java_dev_sogen_app_runtime_SogenNative_nativeSetFrameLimit(JNIEnv* /*env*/, jobject /*self*/, const jlong handle,
                                                                                      const jint frames_per_second)
    {
        get_session(handle).set_frame_limit(static_cast<uint32_t>(std::max(frames_per_second, 0)));
    }

    // Frames presented so far and the longest present interval since the last call, or null while nothing runs.
    JNIEXPORT jlongArray JNICALL Java_dev_sogen_app_runtime_SogenNative_nativeFrameStatistics(JNIEnv* env, jobject /*self*/,
                                                                                              const jlong handle)
    {
        const auto statistics = get_session(handle).frame_statistics();
        if (!statistics)
        {
            return nullptr;
        }

        const std::array<jlong, 2> values{static_cast<jlong>(statistics->frames),
                                          static_cast<jlong>(statistics->worst_present_interval_ns)};
        auto* result = env->NewLongArray(static_cast<jsize>(values.size()));
        if (result)
        {
            env->SetLongArrayRegion(result, 0, static_cast<jsize>(values.size()), values.data());
        }
        return result;
    }

    JNIEXPORT void JNICALL Java_dev_sogen_app_runtime_SogenNative_nativeConsoleInput(JNIEnv* env, jobject /*self*/, const jlong handle,
                                                                                     jbyteArray data)
    {
        std::string bytes(static_cast<size_t>(env->GetArrayLength(data)), '\0');
        env->GetByteArrayRegion(data, 0, static_cast<jsize>(bytes.size()), reinterpret_cast<jbyte*>(bytes.data()));
        get_session(handle).console_input(bytes);
    }

    JNIEXPORT void JNICALL Java_dev_sogen_app_runtime_SogenNative_nativeConsoleResize(JNIEnv* /*env*/, jobject /*self*/, const jlong handle,
                                                                                      const jint columns, const jint rows)
    {
        get_session(handle).console_resize(static_cast<uint16_t>(columns), static_cast<uint16_t>(rows));
    }

    JNIEXPORT void JNICALL Java_dev_sogen_app_runtime_SogenNative_nativeAttachSurface(JNIEnv* env, jobject /*self*/, const jlong handle,
                                                                                      const jlong window, jobject surface)
    {
        auto* native_window = ANativeWindow_fromSurface(env, surface);
        if (!native_window)
        {
            return;
        }

        bool attached = false;
        get_session(handle).with_ui([&](sogen::android::android_ui_backend& ui) {
            ui.attach_surface(static_cast<sogen::hwnd>(window), native_window);
            attached = true;
        });

        if (!attached)
        {
            ANativeWindow_release(native_window);
        }
    }

    JNIEXPORT void JNICALL Java_dev_sogen_app_runtime_SogenNative_nativeDetachSurface(JNIEnv* /*env*/, jobject /*self*/, const jlong handle,
                                                                                      const jlong window)
    {
        get_session(handle).with_ui([&](sogen::android::android_ui_backend& ui) { ui.detach_surface(static_cast<sogen::hwnd>(window)); });
    }

    JNIEXPORT void JNICALL Java_dev_sogen_app_runtime_SogenNative_nativeMouseMove(JNIEnv* /*env*/, jobject /*self*/, const jlong handle,
                                                                                  const jlong window, const jint x, const jint y)
    {
        get_session(handle).with_ui(
            [&](sogen::android::android_ui_backend& ui) { ui.post_mouse_move(static_cast<sogen::hwnd>(window), x, y); });
    }

    JNIEXPORT void JNICALL Java_dev_sogen_app_runtime_SogenNative_nativeMouseMotion(JNIEnv* /*env*/, jobject /*self*/, const jlong handle,
                                                                                    const jlong window, const jint dx, const jint dy)
    {
        get_session(handle).with_ui(
            [&](sogen::android::android_ui_backend& ui) { ui.post_mouse_motion(static_cast<sogen::hwnd>(window), dx, dy); });
    }

    JNIEXPORT void JNICALL Java_dev_sogen_app_runtime_SogenNative_nativeMouseButton(JNIEnv* /*env*/, jobject /*self*/, const jlong handle,
                                                                                    const jlong window, const jint button,
                                                                                    const jboolean down, const jint x, const jint y)
    {
        get_session(handle).with_ui([&](sogen::android::android_ui_backend& ui) {
            ui.post_mouse_button(static_cast<sogen::hwnd>(window), to_mouse_button(button), down == JNI_TRUE, x, y);
        });
    }

    JNIEXPORT void JNICALL Java_dev_sogen_app_runtime_SogenNative_nativeMouseWheel(JNIEnv* /*env*/, jobject /*self*/, const jlong handle,
                                                                                   const jlong window, const jint delta, const jint x,
                                                                                   const jint y)
    {
        get_session(handle).with_ui(
            [&](sogen::android::android_ui_backend& ui) { ui.post_mouse_wheel(static_cast<sogen::hwnd>(window), delta, x, y); });
    }

    JNIEXPORT void JNICALL Java_dev_sogen_app_runtime_SogenNative_nativeKey(JNIEnv* /*env*/, jobject /*self*/, const jlong handle,
                                                                            const jlong window, const jint key_code, const jboolean down)
    {
        get_session(handle).with_ui(
            [&](sogen::android::android_ui_backend& ui) { ui.post_key(static_cast<sogen::hwnd>(window), key_code, down == JNI_TRUE); });
    }

    JNIEXPORT void JNICALL Java_dev_sogen_app_runtime_SogenNative_nativeText(JNIEnv* env, jobject /*self*/, const jlong handle,
                                                                             const jlong window, jstring text)
    {
        const auto characters = to_u16string(*env, text);
        get_session(handle).with_ui(
            [&](sogen::android::android_ui_backend& ui) { ui.post_text(static_cast<sogen::hwnd>(window), characters); });
    }

    JNIEXPORT void JNICALL Java_dev_sogen_app_runtime_SogenNative_nativeClose(JNIEnv* /*env*/, jobject /*self*/, const jlong handle,
                                                                              const jlong window)
    {
        get_session(handle).with_ui([&](sogen::android::android_ui_backend& ui) { ui.post_close(static_cast<sogen::hwnd>(window)); });
    }
}

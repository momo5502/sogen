#pragma once

#include <jni.h>

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

namespace sogen::android
{
    // Calls into Java from a single JVM-attached thread, so emulator threads never attach to the VM and never
    // block on the listener.
    class java_dispatcher
    {
      public:
        using task = std::function<void(JNIEnv&)>;

        explicit java_dispatcher(JavaVM& vm);
        ~java_dispatcher();

        java_dispatcher(const java_dispatcher&) = delete;
        java_dispatcher& operator=(const java_dispatcher&) = delete;
        java_dispatcher(java_dispatcher&&) = delete;
        java_dispatcher& operator=(java_dispatcher&&) = delete;

        void post(task t);

        // Waits until every task posted so far has run.
        void flush();

      private:
        JavaVM* vm_{};
        std::mutex mutex_{};
        std::condition_variable condition_{};
        std::deque<task> tasks_{};
        bool stopping_{};
        std::thread thread_{};

        void run();
    };
}

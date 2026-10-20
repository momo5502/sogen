#include "std_include.hpp"
#include "java_dispatcher.hpp"

#include <future>

namespace sogen::android
{
    java_dispatcher::java_dispatcher(JavaVM& vm)
        : vm_(&vm),
          thread_([this] { this->run(); })
    {
    }

    java_dispatcher::~java_dispatcher()
    {
        {
            const std::lock_guard lock{this->mutex_};
            this->stopping_ = true;
        }

        this->condition_.notify_all();
        this->thread_.join();
    }

    void java_dispatcher::post(task t)
    {
        {
            const std::lock_guard lock{this->mutex_};
            this->tasks_.push_back(std::move(t));
        }

        this->condition_.notify_one();
    }

    void java_dispatcher::flush()
    {
        std::promise<void> done{};
        auto finished = done.get_future();
        this->post([&done](JNIEnv&) { done.set_value(); });
        finished.wait();
    }

    void java_dispatcher::run()
    {
        JNIEnv* env{};
        JavaVMAttachArgs args{.version = JNI_VERSION_1_6, .name = "sogen-dispatch", .group = nullptr};
        if (this->vm_->AttachCurrentThread(&env, &args) != JNI_OK)
        {
            return;
        }

        while (true)
        {
            task current{};

            {
                std::unique_lock lock{this->mutex_};
                this->condition_.wait(lock, [this] { return this->stopping_ || !this->tasks_.empty(); });

                if (this->tasks_.empty())
                {
                    break;
                }

                current = std::move(this->tasks_.front());
                this->tasks_.pop_front();
            }

            current(*env);
            if (env->ExceptionCheck())
            {
                env->ExceptionDescribe();
                env->ExceptionClear();
            }
        }

        this->vm_->DetachCurrentThread();
    }
}

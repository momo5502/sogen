#include "fuzzer.hpp"
#include <cinttypes>

#include "input_generator.hpp"

namespace fuzzer
{
    namespace
    {
        class fuzzing_context
        {
          public:
            fuzzing_context(input_generator& generator, fuzzing_handler& handler)
                : generator(generator),
                  handler(handler)
            {
            }

            void stop()
            {
                this->stop_ = true;
            }

            bool should_stop()
            {
                if (this->stop_)
                {
                    return true;
                }

                if (!handler.stop())
                {
                    return false;
                }

                this->stop_ = true;
                return true;
            }

            input_generator& generator;
            fuzzing_handler& handler;
            std::atomic_uint64_t executions{0};

          private:
            std::atomic_bool stop_{false};
        };

        void perform_fuzzing_iteration(fuzzing_context& context, executer& executer)
        {
            ++context.executions;
            context.generator.access_input([&](const std::span<const uint8_t> input) {
                uint64_t score{0};
                const auto result = executer.execute(input, [&](uint64_t) { ++score; });

                if (result == execution_result::error)
                {
                    printf("Found error!\n");
                    context.stop();
                }

                return score;
            });
        }

        void worker(fuzzing_context& context)
        {
            const auto executer = context.handler.make_executer();

            while (!context.should_stop())
            {
                perform_fuzzing_iteration(context, *executer);
            }
        }

        struct worker_pool
        {
            fuzzing_context* context_{nullptr};
            std::vector<std::thread> workers_{};

            worker_pool(fuzzing_context& context, const size_t concurrency)
                : context_(&context)
            {
                this->workers_.reserve(concurrency);

                for (size_t i = 0; i < concurrency; ++i)
                {
                    this->workers_.emplace_back([&context] { worker(context); });
                }
            }

            ~worker_pool()
            {
                if (this->workers_.empty())
                {
                    return;
                }

                this->context_->stop();

                for (auto& w : this->workers_)
                {
                    w.join();
                }
            }
        };
    }

    void run(fuzzing_handler& handler, const size_t concurrency)
    {
        input_generator generator{};
        fuzzing_context context{generator, handler};
        worker_pool pool{context, concurrency};

        while (!context.should_stop())
        {
            std::this_thread::sleep_for(std::chrono::seconds{1});

            const auto executions = context.executions.exchange(0);
            const auto highest_scorer = context.generator.get_highest_scorer();
            const auto avg_score = context.generator.get_average_score();
            printf("Executions/s: %" PRIu64 " - Score: %" PRIx64 " - Avg: %.3f\n", executions, highest_scorer.score,
                   avg_score);
        }
    }
}

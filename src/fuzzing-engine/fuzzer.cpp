#include "fuzzer.hpp"
#include "input_generator.hpp"

namespace fuzzer
{
	namespace
	{
		class fuzzing_context
		{
		public:
			fuzzing_context(input_generator& generator, fuzzing_handler& handler)
				: generator(generator)
				  , handler(handler)
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

		private:
			std::atomic_bool stop_{false};
		};

		void perform_fuzzing_iteration(const fuzzing_context& context)
		{
			context.generator.access_input([&](const std::span<const uint8_t> input)
			{
				uint64_t score{0};
				context.handler.execute(input, [&](uint64_t)
				{
					++score;
				});

				return score;
			});
		}

		void worker(fuzzing_context& context)
		{
			while (!context.should_stop())
			{
				perform_fuzzing_iteration(context);
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
					this->workers_.emplace_back([&context]
					{
						worker(context);
					});
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
		}
	}
}

#include "input_generator.hpp"

namespace fuzzer
{
	namespace
	{
		constexpr size_t MAX_TOP_SCORER = 20;

		void mutate_input(random_generator& rng, std::vector<uint8_t>& input)
		{
			if (input.empty() || rng.get(10) == 0)
			{
				const auto new_bytes = rng.get_geometric<size_t>() + 1;
				input.resize(input.size() + new_bytes);
			}
			else if (rng.get(10) == 0)
			{
				const auto remove_bytes = rng.get_geometric<size_t>() % input.size();
				input.resize(input.size() - remove_bytes);
			}

			const auto mutations = (rng.get_geometric<size_t>() + 1) % input.size();


			for (size_t i = 0; i < mutations; ++i)
			{
				const auto index = rng.get<size_t>(input.size());
				input[index] = rng.get<uint8_t>();
			}
		}
	}

	input_generator::input_generator() = default;

	std::vector<uint8_t> input_generator::generate_next_input()
	{
		std::vector<uint8_t> input{};
		std::unique_lock lock{this->mutex_};

		if (!this->top_scorer_.empty())
		{
			const auto index = this->rng.get<size_t>() % this->top_scorer_.size();
			input = this->top_scorer_[index].data;
		}

		mutate_input(this->rng, input);

		return input;
	}

	void input_generator::access_input(const std::function<input_handler>& handler)
	{
		auto next_input = this->generate_next_input();
		const auto score = handler(next_input);
		this->store_input_entry({std::move(next_input), score});
	}

	void input_generator::store_input_entry(input_entry entry)
	{
		std::unique_lock lock{this->mutex_};

		if (entry.score < this->lowest_score && this->rng.get(40) != 0)
		{
			return;
		}

		const auto score = entry.score;

		if (this->top_scorer_.size() < MAX_TOP_SCORER)
		{
			this->top_scorer_.emplace_back(std::move(entry));
		}
		else
		{
			const auto index = this->rng.get<size_t>() % this->top_scorer_.size();
			this->top_scorer_[index] = std::move(entry);
		}

		this->lowest_score = score;
		if (score < this->lowest_score)
		{
			return;
		}

		for (const auto& e : this->top_scorer_)
		{
			if (e.score < this->lowest_score)
			{
				this->lowest_score = e.score;
			}
		}
	}
}

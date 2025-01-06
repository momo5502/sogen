#pragma once
#include <mutex>
#include <vector>
#include <optional>
#include <functional>

#include "random_generator.hpp"

namespace fuzzer
{
    using input_score = uint64_t;
    using input_handler = input_score(std::span<const uint8_t>);

    struct input_entry
    {
        std::vector<uint8_t> data{};
        input_score score{};
    };

    class input_generator
    {
      public:
        input_generator();

        void access_input(const std::function<input_handler>& handler);

        input_entry get_highest_scorer();
        double get_average_score();

      private:
        std::mutex mutex_{};
        random_generator rng{};

        std::vector<input_entry> top_scorer_{};
        input_score lowest_score{0};
        size_t lowest_scorer{0};

        input_entry highest_scorer_{};

        std::vector<uint8_t> generate_next_input();

        void store_input_entry(input_entry entry);
    };
}

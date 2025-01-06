#pragma once
#include <span>
#include <random>
#include <cstdint>
#include <cstring>

namespace fuzzer
{
    class random_generator
    {
      public:
        random_generator();

        void fill(std::span<uint8_t> data);
        void fill(void* data, size_t size);

        template <typename T>
            requires(std::is_trivially_copyable_v<T>)
        T get()
        {
            T value{};
            this->fill(&value, sizeof(value));
            return value;
        }

        template <typename T>
        T get(const T& max)
        {
            return this->get<T>() % max;
        }

        template <typename T>
        T get(T min, T max)
        {
            if (max < min)
            {
                std::swap(max, min);
            }

            const auto diff = max - min;

            return (this->get<T>() % diff) + min;
        }

        template <typename T>
        T get_geometric()
        {
            T value{0};

            while (this->get<bool>())
            {
                ++value;
            }

            return value;
        }

      private:
        std::mt19937 rng_;
        std::uniform_int_distribution<std::mt19937::result_type> distribution_{};

        std::mt19937::result_type generate_number();
    };

    template <>
    inline bool random_generator::get<bool>()
    {
        return (this->generate_number() & 1) != 0;
    }
}

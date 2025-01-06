#include "random_generator.hpp"
#include <cstring>

namespace fuzzer
{
    random_generator::random_generator()
        : rng_(std::random_device()())
    {
    }

    std::mt19937::result_type random_generator::generate_number()
    {
        return this->distribution_(this->rng_);
    }

    void random_generator::fill(void* data, const size_t size)
    {
        this->fill(std::span(static_cast<uint8_t*>(data), size));
    }

    void random_generator::fill(std::span<uint8_t> data)
    {
        size_t i = 0;
        while (i < data.size())
        {
            const auto number = this->generate_number();

            const auto remaining_data = data.size() - i;
            const auto data_to_fill = std::min(remaining_data, sizeof(number));

            memcpy(data.data() + i, &number, data_to_fill);
            i += data_to_fill;
        }
    }
}

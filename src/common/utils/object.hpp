#pragma once

namespace utils
{
    struct object
    {
        object() = default;
        virtual ~object() = default;

        object(object&&) = default;
        object(const object&) = default;
        object& operator=(object&&) = default;
        object& operator=(const object&) = default;
    };

    template <typename T>
    void reset_object_with_delayed_destruction(T& obj)
    {
        T new_obj{};
        const auto old = std::move(obj);
        obj = std::move(new_obj);
    }
}

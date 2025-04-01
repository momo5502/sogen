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
}

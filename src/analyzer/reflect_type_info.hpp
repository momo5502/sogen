#pragma once

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wtautological-compare"
#endif

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-private-field"
#endif

#include "reflect_extension.hpp"
#include <reflect>

#if defined(__clang__)
#pragma clang diagnostic pop
#endif

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

template <typename T>
class reflect_type_info
{
  public:
    reflect_type_info()
    {
        this->type_name_ = reflect::type_name<T>();

        reflect::for_each<T>([this](auto I) {
            const auto member_name = reflect::member_name<I, T>();
            const auto member_offset = reflect::offset_of<I, T>();

            this->members_[member_offset] = member_name;
        });
    }

    std::string get_member_name(const size_t offset) const
    {
        size_t last_offset{};
        std::string_view last_member{};

        for (const auto& member : this->members_)
        {
            if (offset == member.first)
            {
                return member.second;
            }

            if (offset < member.first)
            {
                const auto diff = offset - last_offset;
                return std::string(last_member) + "+" + std::to_string(diff);
            }

            last_offset = member.first;
            last_member = member.second;
        }

        return "<N/A>";
    }

    const std::string& get_type_name() const
    {
        return this->type_name_;
    }

  private:
    std::string type_name_{};
    std::map<size_t, std::string> members_{};
};

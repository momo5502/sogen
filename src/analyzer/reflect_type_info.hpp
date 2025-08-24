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

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4189)
#pragma warning(disable : 4308)
#endif

#include "reflect_extension.hpp"
#include <reflect>

#ifdef _MSC_VER
#pragma warning(pop)
#endif

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
            const auto member_size = reflect::size_of<I, T>();

            this->members_[member_offset] = std::make_pair(std::string(member_name), member_size);
        });
    }

    std::string get_member_name(const size_t offset) const
    {
        const auto info = this->get_member_info(offset);
        if (!info.has_value())
        {
            return "<N/A>";
        }

        return info->get_diff_name(offset);
    }

    struct member_info
    {
        std::string name{};
        size_t offset{};
        size_t size{};

        std::string get_diff_name(const size_t access) const
        {
            const auto diff = access - this->offset;
            if (diff == 0)
            {
                return this->name;
            }

            return this->name + "+" + std::to_string(diff);
        }
    };

    std::optional<member_info> get_member_info(const size_t offset) const
    {
        auto entry = this->members_.upper_bound(offset);
        if (entry == this->members_.begin())
        {
            return std::nullopt;
        }

        --entry;

        if (entry->first + entry->second.second <= offset)
        {
            return std::nullopt;
        }

        return member_info{
            .name = entry->second.first,
            .offset = entry->first,
            .size = entry->second.second,
        };
    }

    const std::string& get_type_name() const
    {
        return this->type_name_;
    }

  private:
    std::string type_name_{};
    std::map<size_t, std::pair<std::string, size_t>> members_{};
};

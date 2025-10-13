#pragma once
#include <string>

namespace utils::wildcard
{
    inline bool is_wildcard(char16_t c)
    {
        return c == '*' || c == '?' || c == '>' || c == '<' || c == '\"';
    }

    inline bool has_wildcard(const std::u16string_view mask)
    {
        return std::ranges::any_of(mask, is_wildcard);
    }

    inline bool match_filename(std::u16string_view name, std::u16string_view mask)
    {
        if (mask.empty() || mask == u"*" || mask == u"*.*")
        {
            return true;
        }

        size_t name_pos = 0;
        size_t mask_pos = 0;

        size_t star_mask_pos = std::u16string_view::npos;
        size_t star_name_pos = 0;

        while (name_pos < name.size())
        {
            if (mask_pos < mask.size())
            {
                char16_t mask_char = mask[mask_pos];
                char16_t name_char = name[name_pos];

                bool char_matches = false;
                if (mask_char == u'?' || mask_char == u'>')
                {
                    char_matches = true;
                }
                else if (mask_char == u'"')
                {
                    char_matches = name_char == u'.';
                }
                else
                {
                    char_matches = string::char_to_lower(name_char) == string::char_to_lower(mask_char);
                }

                // Advance if current characters match
                if (char_matches)
                {
                    name_pos++;
                    mask_pos++;
                    continue;
                }

                // If this is a wildcard, skip all consecutive wildcards and save position for backtracking
                if (mask[mask_pos] == u'*' || mask[mask_pos] == u'<')
                {
                    mask_pos++;
                    while (mask_pos < mask.size() && (mask[mask_pos] == u'*' || mask[mask_pos] == u'<'))
                    {
                        mask_pos++;
                    }

                    if (mask_pos == mask.size())
                    {
                        // There is no need to continue because all that remained were star masks.
                        return true;
                    }

                    star_mask_pos = mask_pos;
                    star_name_pos = name_pos;
                    continue;
                }
            }

            // The current characters didn't match...
            // If we had a wildcard earlier, backtrack to it and try to match at the next position
            if (star_mask_pos != std::u16string_view::npos)
            {
                mask_pos = star_mask_pos;
                name_pos = ++star_name_pos;
                continue;
            }

            return false;
        }

        // Skip any remaining wildcards in the mask
        while (mask_pos < mask.size() && (mask[mask_pos] == u'*' || mask[mask_pos] == u'<'))
        {
            mask_pos++;
        }

        return mask_pos == mask.size();
    }
}

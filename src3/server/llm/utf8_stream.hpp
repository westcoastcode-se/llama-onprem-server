#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>

namespace Utf8Util
{

// Returns the byte length of the longest prefix of `s` that ends on a complete UTF-8 codepoint.
inline size_t get_complete_utf8_prefix_len(std::string_view s)
{
    if (s.empty())
        return 0;

    const size_t len = s.size();
    const size_t max_lookback = std::min(static_cast<size_t>(4), len);

    for (size_t i = 1; i <= max_lookback; ++i)
    {
        unsigned char c = static_cast<unsigned char>(s[len - i]);
        if ((c & 0xC0) != 0x80)
        {
            size_t expected = 1;
            if ((c & 0xF8) == 0xF0)
            {
                expected = 4;
            }
            else if ((c & 0xF0) == 0xE0)
            {
                expected = 3;
            }
            else if ((c & 0xE0) == 0xC0)
            {
                expected = 2;
            }
            else
            {
                expected = 1;
            }

            if (i < expected)
            {
                return len - i;
            }
            return len;
        }
    }

    return len;
}

class StreamBuffer
{
  public:
    StreamBuffer() = default;

    std::string process(std::string_view piece)
    {
        buf_.append(piece);
        size_t valid_len = get_complete_utf8_prefix_len(buf_);
        if (valid_len == 0)
        {
            return "";
        }
        std::string ready = buf_.substr(0, valid_len);
        buf_.erase(0, valid_len);
        return ready;
    }

    std::string flush()
    {
        if (buf_.empty())
            return "";
        std::string remaining = std::move(buf_);
        buf_.clear();
        return remaining;
    }

    void reset()
    {
        buf_.clear();
    }

    [[nodiscard]] bool empty() const
    {
        return buf_.empty();
    }

  private:
    std::string buf_;
};

} // namespace Utf8Util

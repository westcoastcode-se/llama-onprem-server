#pragma once

#include "common/devstral_call.hpp"

#include <array>
#include <string>
#include <string_view>

// Assistant text as the user should see it. Tool-call XML, question blocks, and
// the same kind of protocol markup stay buffered until they close, then drop.
struct VisibleText
{
    std::string feed(std::string_view piece)
    {
        pending.append(piece);
        return drain(false);
    }

    std::string finish()
    {
        return drain(true);
    }

  private:
    struct Tag
    {
        std::string_view open;
        std::string_view close;
    };

    std::string pending;
    bool hiding = false;
    bool hiding_devstral = false;
    std::string close;

    static bool ieq(std::string_view text, std::size_t pos, std::string_view literal)
    {
        if (literal.size() > text.size() || pos > text.size() - literal.size())
        {
            return false;
        }
        for (std::size_t i = 0; i < literal.size(); ++i)
        {
            unsigned char left = static_cast<unsigned char>(text[pos + i]);
            unsigned char right = static_cast<unsigned char>(literal[i]);
            if (left >= 'A' && left <= 'Z')
            {
                left = static_cast<unsigned char>(left - 'A' + 'a');
            }
            if (right >= 'A' && right <= 'Z')
            {
                right = static_cast<unsigned char>(right - 'A' + 'a');
            }
            if (left != right)
            {
                return false;
            }
        }
        return true;
    }

    static std::size_t find_ieq(std::string_view text, std::string_view literal)
    {
        if (literal.empty() || literal.size() > text.size())
        {
            return std::string_view::npos;
        }
        const std::size_t last = text.size() - literal.size();
        for (std::size_t i = 0; i <= last; ++i)
        {
            if (ieq(text, i, literal))
            {
                return i;
            }
        }
        return std::string_view::npos;
    }

    static const std::array<Tag, 9> &tags()
    {
        static constexpr std::array<Tag, 9> kTags = {{
            {"<tool_calls>", "</tool_calls>"},
            {"<tool_call>", "</tool_call>"},
            {"<tool_response>", "</tool_response>"},
            {"<question>", "</question>"},
            {"<answer>", "</answer>"},
            {"<｜tool▁calls▁begin｜>", "<｜tool▁calls▁end｜>"},
            {"<｜tool▁call▁begin｜>", "<｜tool▁call▁end｜>"},
            {"<function=", "</function>"},
            {"<parameter=", "</parameter>"},
        }};
        return kTags;
    }

    static std::size_t held_prefix(std::string_view text)
    {
        std::size_t held = 0;
        const auto consider = [&](std::string_view open) {
            if (open.size() < 2)
            {
                return;
            }
            const std::size_t max = std::min(text.size(), open.size() - 1);
            for (std::size_t size = max; size > held; --size)
            {
                if (ieq(open, 0, text.substr(text.size() - size)))
                {
                    held = size;
                    break;
                }
            }
        };
        for (const Tag &tag : tags())
        {
            consider(tag.open);
        }
        consider(kDevstralToolCalls);
        return held;
    }

    static const Tag *earliest(std::string_view text, std::size_t &pos)
    {
        const Tag *found = nullptr;
        pos = std::string_view::npos;
        for (const Tag &tag : tags())
        {
            const std::size_t at = find_ieq(text, tag.open);
            if (at < pos)
            {
                pos = at;
                found = &tag;
            }
        }
        return found;
    }

    std::string drain(bool end)
    {
        std::string out;
        while (!pending.empty())
        {
            if (hiding)
            {
                const std::size_t at = find_ieq(pending, close);
                if (at == std::string_view::npos)
                {
                    if (end)
                    {
                        pending.clear();
                        hiding = false;
                    }
                    break;
                }
                pending.erase(0, at + close.size());
                hiding = false;
                while (!pending.empty() && (pending.front() == '\n' || pending.front() == '\r'))
                {
                    pending.erase(pending.begin());
                }
                continue;
            }
            if (hiding_devstral)
            {
                const std::size_t call_end = devstral_call_end(pending, 0);
                if (call_end == std::string_view::npos)
                {
                    if (end)
                    {
                        pending.clear();
                        hiding_devstral = false;
                    }
                    break;
                }
                pending.erase(0, call_end);
                hiding_devstral = false;
                while (!pending.empty() && (pending.front() == '\n' || pending.front() == '\r'))
                {
                    pending.erase(pending.begin());
                }
                continue;
            }

            std::size_t pos = 0;
            const Tag *tag = earliest(pending, pos);
            const std::size_t devstral = pending.find(kDevstralToolCalls);
            if (devstral != std::string_view::npos && (tag == nullptr || devstral <= pos))
            {
                std::size_t cut = devstral;
                bool broke = false;
                while (cut > 0 && (pending[cut - 1] == '\n' || pending[cut - 1] == '\r'))
                {
                    broke = true;
                    --cut;
                }
                out.append(pending, 0, cut);
                if (broke)
                {
                    out.push_back('\n');
                }
                pending.erase(0, devstral);
                hiding_devstral = true;
                continue;
            }
            if (tag == nullptr)
            {
                const std::size_t hold = held_prefix(pending);
                std::size_t emit = pending.size() - hold;
                if (!end)
                {
                    while (emit > 0 && (pending[emit - 1] == '\n' || pending[emit - 1] == '\r'))
                    {
                        --emit;
                    }
                }
                out.append(pending, 0, emit);
                pending.erase(0, emit);
                if (end)
                {
                    pending.clear();
                }
                break;
            }

            std::size_t cut = pos;
            bool broke = false;
            while (cut > 0 && (pending[cut - 1] == '\n' || pending[cut - 1] == '\r'))
            {
                broke = true;
                --cut;
            }
            out.append(pending, 0, cut);
            if (broke)
            {
                out.push_back('\n');
            }
            pending.erase(0, pos + tag->open.size());
            hiding = true;
            close.assign(tag->close);
        }
        return out;
    }
};

#include "cli/tui/visible_text.hpp"

#include "common/devstral_call.hpp"

#include <algorithm>
#include <array>
#include <stdexcept>
#include <utility>

namespace
{

bool is_break(char ch)
{
    return ch == '\n' || ch == '\r';
}

void drop_leading_breaks(std::string &text)
{
    std::size_t count = 0;
    while (count < text.size() && is_break(text[count]))
    {
        ++count;
    }
    text.erase(0, count);
}

bool ieq(std::string_view text, std::size_t pos, std::string_view literal)
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

std::size_t find_ieq(std::string_view text, std::string_view literal)
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

// A suffix shorter than the open. The full open is found by find_open instead.
std::size_t held_open_prefix(std::string_view text, std::string_view open)
{
    if (open.size() < 2)
    {
        return 0;
    }
    const std::size_t max = std::min(text.size(), open.size() - 1);
    for (std::size_t size = max; size > 0; --size)
    {
        if (ieq(open, 0, text.substr(text.size() - size)))
        {
            return size;
        }
    }
    return 0;
}

constexpr std::array<PairedTagMarkup::Tag, 4> kQwenTags = {{
    {"<tool_calls>", "</tool_calls>"},
    {"<tool_call>", "</tool_call>"},
    {"<function=", "</function>"},
    {"<parameter=", "</parameter>"},
}};

constexpr std::array<PairedTagMarkup::Tag, 2> kDeepseekTags = {{
    {"<｜tool▁calls▁begin｜>", "<｜tool▁calls▁end｜>"},
    {"<｜tool▁call▁begin｜>", "<｜tool▁call▁end｜>"},
}};

constexpr std::array<PairedTagMarkup::Tag, 3> kProtocolTags = {{
    {"<tool_response>", "</tool_response>"},
    {"<question>", "</question>"},
    {"<answer>", "</answer>"},
}};

} // namespace

Markup::~Markup() = default;

std::size_t Markup::span_end(std::string_view) const
{
    return std::string_view::npos;
}

const PairedTagMarkup::Tag *PairedTagMarkup::longest_at(std::string_view text, std::size_t pos) const
{
    const Tag *found = nullptr;
    for (const Tag &tag : tags_)
    {
        if (!ieq(text, pos, tag.open))
        {
            continue;
        }
        if (found == nullptr || tag.open.size() > found->open.size())
        {
            found = &tag;
        }
    }
    return found;
}

std::size_t PairedTagMarkup::find_open(std::string_view text) const
{
    std::size_t best = std::string_view::npos;
    for (const Tag &tag : tags_)
    {
        const std::size_t at = find_ieq(text, tag.open);
        if (at < best)
        {
            best = at;
        }
    }
    return best;
}

std::size_t PairedTagMarkup::open_length(std::string_view text) const
{
    const Tag *tag = longest_at(text, 0);
    if (tag == nullptr)
    {
        return 0;
    }
    return tag->open.size();
}

std::size_t PairedTagMarkup::held_prefix(std::string_view text) const
{
    std::size_t held = 0;
    for (const Tag &tag : tags_)
    {
        held = std::max(held, held_open_prefix(text, tag.open));
    }
    return held;
}

std::optional<Markup::OpenSpan> PairedTagMarkup::classify(std::string_view text) const
{
    const Tag *tag = longest_at(text, 0);
    if (tag == nullptr)
    {
        return std::nullopt;
    }
    return OpenSpan{tag->close, tag->open.size(), false};
}

QwenMarkup::QwenMarkup() : PairedTagMarkup(kQwenTags)
{
}

DeepseekMarkup::DeepseekMarkup() : PairedTagMarkup(kDeepseekTags)
{
}

ProtocolMarkup::ProtocolMarkup() : PairedTagMarkup(kProtocolTags)
{
}

std::size_t DevstralMarkup::find_open(std::string_view text) const
{
    return text.find(kDevstralToolCalls);
}

std::size_t DevstralMarkup::open_length(std::string_view text) const
{
    if (!text.starts_with(kDevstralToolCalls))
    {
        return 0;
    }
    return kDevstralToolCalls.size();
}

std::size_t DevstralMarkup::held_prefix(std::string_view text) const
{
    return held_open_prefix(text, kDevstralToolCalls);
}

std::optional<Markup::OpenSpan> DevstralMarkup::classify(std::string_view text) const
{
    if (!text.starts_with(kDevstralToolCalls))
    {
        return std::nullopt;
    }
    return OpenSpan{{}, 0, true};
}

std::size_t DevstralMarkup::span_end(std::string_view text) const
{
    return devstral_call_end(text, 0);
}

std::string VisibleText::feed(std::string_view piece)
{
    pending_.append(piece);
    return drain(false);
}

std::string VisibleText::finish()
{
    return drain(true);
}

void VisibleText::stop_hiding()
{
    hiding_ = false;
    whole_ = false;
    family_ = Family::None;
    close_ = {};
}

std::array<const Markup *, 4> VisibleText::families() const
{
    return {&protocol_, &qwen_, &deepseek_, &devstral_};
}

VisibleText::Family VisibleText::family_of(const Markup *markup) const
{
    if (markup == &protocol_)
    {
        return Family::Protocol;
    }
    if (markup == &qwen_)
    {
        return Family::Qwen;
    }
    if (markup == &deepseek_)
    {
        return Family::Deepseek;
    }
    if (markup == &devstral_)
    {
        return Family::Devstral;
    }
    throw std::logic_error("visible text family");
}

const Markup &VisibleText::markup(Family family) const
{
    switch (family)
    {
    case Family::Protocol:
        return protocol_;
    case Family::Qwen:
        return qwen_;
    case Family::Deepseek:
        return deepseek_;
    case Family::Devstral:
        return devstral_;
    case Family::None:
        break;
    }
    throw std::logic_error("visible text family");
}

std::size_t VisibleText::held(std::string_view text) const
{
    std::size_t size = 0;
    for (const Markup *family : families())
    {
        size = std::max(size, family->held_prefix(text));
    }
    return size;
}

std::string VisibleText::drain(bool end)
{
    std::string out;
    while (!pending_.empty())
    {
        if (hiding_)
        {
            std::size_t span = std::string_view::npos;
            if (whole_)
            {
                span = markup(family_).span_end(pending_);
            }
            else
            {
                const std::size_t at = find_ieq(pending_, close_);
                if (at != std::string_view::npos)
                {
                    span = at + close_.size();
                }
            }
            if (span == std::string_view::npos || span == 0)
            {
                if (end)
                {
                    pending_.clear();
                    stop_hiding();
                }
                break;
            }
            pending_.erase(0, span);
            stop_hiding();
            drop_leading_breaks(pending_);
            continue;
        }

        std::size_t pos = std::string_view::npos;
        for (const Markup *family : families())
        {
            const std::size_t at = family->find_open(pending_);
            if (at < pos)
            {
                pos = at;
            }
        }
        if (pos == std::string_view::npos)
        {
            const std::size_t hold = held(pending_);
            std::size_t emit = pending_.size() - hold;
            if (!end)
            {
                while (emit > 0 && is_break(pending_[emit - 1]))
                {
                    --emit;
                }
            }
            out.append(pending_, 0, emit);
            pending_.erase(0, emit);
            if (end)
            {
                pending_.clear();
            }
            break;
        }

        const Markup *chosen = nullptr;
        std::size_t length = 0;
        for (const Markup *family : families())
        {
            const std::size_t open = family->open_length(pending_.substr(pos));
            if (open > length)
            {
                chosen = family;
                length = open;
            }
        }
        if (chosen == nullptr)
        {
            out.append(pending_, 0, 1);
            pending_.erase(0, 1);
            continue;
        }

        std::size_t cut = pos;
        bool broke = false;
        while (cut > 0 && is_break(pending_[cut - 1]))
        {
            broke = true;
            --cut;
        }
        out.append(pending_, 0, cut);
        if (broke)
        {
            out.push_back('\n');
        }
        pending_.erase(0, pos);
        const std::optional<Markup::OpenSpan> span = chosen->classify(pending_);
        if (!span)
        {
            out.append(pending_, 0, 1);
            pending_.erase(0, 1);
            continue;
        }
        if (span->whole)
        {
            hiding_ = true;
            whole_ = true;
            family_ = family_of(chosen);
            close_ = {};
            continue;
        }
        pending_.erase(0, span->skip);
        hiding_ = true;
        whole_ = false;
        family_ = Family::None;
        close_ = span->close;
    }
    return out;
}

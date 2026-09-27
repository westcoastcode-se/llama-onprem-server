#include "tool_history.hpp"

#include <algorithm>
#include <cctype>
#include <format>
#include <string>
#include <string_view>

namespace
{

constexpr std::size_t kKeepChars = 240;
constexpr std::size_t kKeepLines = 3;
constexpr std::size_t kDetailChars = 160;
constexpr std::string_view kClose = "</tool_response>";

std::string_view trim_edges(std::string_view text)
{
    while (!text.empty() && (text.front() == ' ' || text.front() == '\n' || text.front() == '\r' || text.front() == '\t'))
    {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\n' || text.back() == '\r' || text.back() == '\t'))
    {
        text.remove_suffix(1);
    }
    return text;
}

std::size_t line_count(std::string_view text)
{
    if (text.empty())
    {
        return 0;
    }
    std::size_t count = 1;
    for (const char c : text)
    {
        if (c == '\n')
        {
            ++count;
        }
    }
    if (text.back() == '\n')
    {
        --count;
    }
    return count;
}

bool safe_name(std::string_view name)
{
    if (name.empty() || name.size() > 64)
    {
        return false;
    }
    for (const unsigned char c : name)
    {
        if (std::isalnum(c) == 0 && c != '_' && c != '-')
        {
            return false;
        }
    }
    return true;
}

std::string sanitize_detail(std::string_view text)
{
    std::string out;
    out.reserve(std::min(text.size(), kDetailChars));
    for (const char c : text)
    {
        if (c == '\n' || c == '\r')
        {
            break;
        }
        if (out.size() == kDetailChars)
        {
            break;
        }
        if (c == '"' || c == '<' || c == '>' || c == '\t')
        {
            out.push_back(' ');
        }
        else
        {
            out.push_back(c);
        }
    }
    while (!out.empty() && out.back() == ' ')
    {
        out.pop_back();
    }
    while (!out.empty() && out.front() == ' ')
    {
        out.erase(out.begin());
    }
    return out;
}

std::string_view attribute(std::string_view tag, std::string_view key)
{
    const std::string needle = std::string(key) + "=\"";
    const auto pos = tag.find(needle);
    if (pos == std::string_view::npos)
    {
        return {};
    }
    const auto start = pos + needle.size();
    const auto end = tag.find('"', start);
    if (end == std::string_view::npos)
    {
        return {};
    }
    return tag.substr(start, end - start);
}

std::string_view line_window(std::string_view body)
{
    const auto nl = body.find('\n');
    const auto first = body.substr(0, nl == std::string_view::npos ? body.size() : nl);
    if (first.size() > 80 || !first.starts_with("lines ") || first.find(" of ") == std::string_view::npos)
    {
        return {};
    }
    return first;
}

bool already_record(std::string_view body, std::string_view name)
{
    if (body.find('\n') != std::string_view::npos)
    {
        return false;
    }
    if (body.starts_with("omitted "))
    {
        return true;
    }
    const std::string_view label = name.empty() ? std::string_view("tool") : name;
    if (!body.starts_with(label))
    {
        return false;
    }
    return body.find(", lines ") != std::string_view::npos || body.find(" lines") != std::string_view::npos;
}

std::string record_line(std::string_view name, std::string_view detail, std::string_view body)
{
    const std::string label = name.empty() ? "tool" : std::string(name);
    const std::string_view window = line_window(body);
    if (!window.empty())
    {
        if (detail.empty())
        {
            return std::format("{}, {}", label, window);
        }
        return std::format("{} {}, {}", label, detail, window);
    }
    const std::size_t lines = line_count(body);
    if (detail.empty())
    {
        return std::format("{}, {} lines", label, lines);
    }
    return std::format("{} {}, {} lines", label, detail, lines);
}

} // namespace

std::string tool_response_open(std::string_view name, std::string_view detail)
{
    const bool named = safe_name(name);
    const std::string clean = sanitize_detail(detail);
    if (!named && clean.empty())
    {
        return "<tool_response>";
    }
    std::string tag = "<tool_response";
    if (named)
    {
        tag += std::format(" name=\"{}\"", name);
    }
    if (!clean.empty())
    {
        tag += std::format(" detail=\"{}\"", clean);
    }
    tag.push_back('>');
    return tag;
}

std::string shrink_tool_responses(std::string_view text)
{
    constexpr std::string_view kOpen = "<tool_response";
    if (text.find(kOpen) == std::string_view::npos)
    {
        return std::string(text);
    }
    std::string out;
    out.reserve(text.size());
    std::size_t pos = 0;
    while (pos < text.size())
    {
        const auto start = text.find(kOpen, pos);
        if (start == std::string_view::npos)
        {
            out.append(text.substr(pos));
            break;
        }
        out.append(text.substr(pos, start - pos));
        const auto tag_end = text.find('>', start);
        const auto end = text.find(kClose, start);
        if (tag_end == std::string_view::npos || end == std::string_view::npos || tag_end > end)
        {
            out.append(text.substr(start));
            break;
        }
        const std::string_view tag = text.substr(start, tag_end - start + 1);
        const std::string_view body = trim_edges(text.substr(tag_end + 1, end - (tag_end + 1)));
        const std::string_view name = attribute(tag, "name");
        const std::string_view detail = attribute(tag, "detail");
        const bool small = body.size() <= kKeepChars && line_count(body) <= kKeepLines;
        if (already_record(body, name) || small)
        {
            out.append(text.substr(start, end + kClose.size() - start));
        }
        else
        {
            out += tag;
            out.push_back('\n');
            out += record_line(name, detail, body);
            out.push_back('\n');
            out += kClose;
        }
        pos = end + kClose.size();
    }
    return out;
}

#include "cli/project/project_detail.hpp"

namespace project_detail
{
namespace
{

constexpr int kPatternCap = 40;

[[nodiscard]] std::string_view trim_gitignore_line(std::string_view line)
{
    if (!line.empty() && line.back() == '\r')
    {
        line.remove_suffix(1);
    }
    while (!line.empty() && line.back() == ' ')
    {
        std::size_t slashes = 0;
        for (std::size_t i = line.size() - 1; i > 0 && line[i - 1] == '\\'; --i)
        {
            ++slashes;
        }
        if (slashes % 2 == 1)
        {
            break;
        }
        line.remove_suffix(1);
    }
    return line;
}

[[nodiscard]] bool class_matches(std::string_view klass, char c)
{
    if (klass.size() < 3 || klass.front() != '[' || klass.back() != ']')
    {
        return false;
    }
    std::size_t i = 1;
    bool negate = false;
    if (klass[i] == '!' || klass[i] == '^')
    {
        negate = true;
        ++i;
    }
    const auto uc = static_cast<unsigned char>(c);
    bool matched = false;
    while (i + 1 < klass.size())
    {
        const auto first = static_cast<unsigned char>(klass[i]);
        if (i + 3 < klass.size() && klass[i + 1] == '-' && klass[i + 2] != ']')
        {
            const auto last = static_cast<unsigned char>(klass[i + 2]);
            if (first <= last && uc >= first && uc <= last)
            {
                matched = true;
            }
            i += 3;
            continue;
        }
        if (uc == first)
        {
            matched = true;
        }
        ++i;
    }
    return negate ? !matched : matched;
}

[[nodiscard]] bool glob_match(std::string_view pattern, std::string_view text)
{
    std::size_t pi = 0;
    std::size_t ti = 0;
    while (pi < pattern.size())
    {
        if (pattern[pi] == '\\' && pi + 1 < pattern.size())
        {
            if (ti >= text.size() || text[ti] != pattern[pi + 1])
            {
                return false;
            }
            pi += 2;
            ++ti;
            continue;
        }
        if (pattern[pi] == '*')
        {
            while (pi < pattern.size() && pattern[pi] == '*')
            {
                ++pi;
            }
            if (pi == pattern.size())
            {
                return true;
            }
            for (std::size_t skip = ti; skip <= text.size(); ++skip)
            {
                if (glob_match(pattern.substr(pi), text.substr(skip)))
                {
                    return true;
                }
            }
            return false;
        }
        if (ti >= text.size())
        {
            return false;
        }
        if (pattern[pi] == '?')
        {
            ++pi;
            ++ti;
            continue;
        }
        if (pattern[pi] == '[')
        {
            const auto close = pattern.find(']', pi + 1);
            if (close == std::string_view::npos)
            {
                if (text[ti] != '[')
                {
                    return false;
                }
                ++pi;
                ++ti;
                continue;
            }
            if (!class_matches(pattern.substr(pi, close - pi + 1), text[ti]))
            {
                return false;
            }
            pi = close + 1;
            ++ti;
            continue;
        }
        if (pattern[pi] != text[ti])
        {
            return false;
        }
        ++pi;
        ++ti;
    }
    return ti == text.size();
}

[[nodiscard]] std::vector<std::string_view> split_path(std::string_view path)
{
    std::vector<std::string_view> parts;
    std::size_t start = 0;
    while (start < path.size())
    {
        const auto slash = path.find('/', start);
        if (slash == std::string_view::npos)
        {
            parts.push_back(path.substr(start));
            break;
        }
        if (slash > start)
        {
            parts.push_back(path.substr(start, slash - start));
        }
        start = slash + 1;
    }
    return parts;
}

[[nodiscard]] std::vector<std::string> split_pattern(std::string_view pattern)
{
    std::vector<std::string> parts;
    std::string current;
    auto push = [&]() {
        if (!current.empty())
        {
            parts.push_back(std::move(current));
            current.clear();
        }
    };
    for (std::size_t i = 0; i < pattern.size(); ++i)
    {
        if (pattern[i] == '\\' && i + 1 < pattern.size())
        {
            current.push_back('\\');
            current.push_back(pattern[++i]);
            continue;
        }
        if (pattern[i] == '/')
        {
            push();
            continue;
        }
        current.push_back(pattern[i]);
    }
    push();
    return parts;
}

[[nodiscard]] bool segments_match(const std::vector<std::string> &pattern, std::size_t pi,
                                  const std::vector<std::string_view> &path, std::size_t si)
{
    while (pi < pattern.size())
    {
        if (pattern[pi] == "**")
        {
            if (pi + 1 == pattern.size())
            {
                return si < path.size();
            }
            for (std::size_t skip = si; skip <= path.size(); ++skip)
            {
                if (segments_match(pattern, pi + 1, path, skip))
                {
                    return true;
                }
            }
            return false;
        }
        if (si >= path.size() || !glob_match(pattern[pi], path[si]))
        {
            return false;
        }
        ++pi;
        ++si;
    }
    return si == path.size();
}

[[nodiscard]] bool rule_matches(const IgnoreRule &rule, std::string_view relative, bool directory)
{
    const auto path = split_path(relative);
    if (path.empty())
    {
        return false;
    }
    if (!rule.anchored)
    {
        if (rule.directory_only && !directory)
        {
            return false;
        }
        return glob_match(rule.base, path.back());
    }
    if (!segments_match(rule.parts, 0, path, 0))
    {
        return false;
    }
    return !rule.directory_only || directory;
}

[[nodiscard]] bool direct_ignored(const std::vector<IgnoreRule> &rules, std::string_view relative, bool directory)
{
    bool skip = false;
    bool seen = false;
    for (const auto &rule : rules)
    {
        if (rule_matches(rule, relative, directory))
        {
            skip = !rule.negate;
            seen = true;
        }
    }
    return seen && skip;
}

void add_ignore_rule(GitIgnore &ignore, std::string_view line)
{
    line = trim_gitignore_line(line);
    if (line.empty() || line.front() == '#')
    {
        return;
    }
    IgnoreRule rule;
    auto pattern = line;
    if (pattern.front() == '!')
    {
        rule.negate = true;
        pattern.remove_prefix(1);
    }
    else if (pattern.starts_with("\\#") || pattern.starts_with("\\!"))
    {
        pattern.remove_prefix(1);
    }
    if (pattern.empty())
    {
        return;
    }
    if (pattern.back() == '/')
    {
        rule.directory_only = true;
        pattern.remove_suffix(1);
    }
    if (pattern.empty())
    {
        return;
    }
    if (pattern.front() == '/')
    {
        rule.anchored = true;
        pattern.remove_prefix(1);
    }
    if (pattern.find('/') != std::string_view::npos)
    {
        rule.anchored = true;
    }
    if (pattern.empty())
    {
        return;
    }
    if (rule.anchored)
    {
        rule.parts = split_pattern(pattern);
        if (rule.parts.empty())
        {
            return;
        }
    }
    else
    {
        rule.base = std::string(pattern);
    }
    ignore.patterns.emplace_back(line);
    ignore.rules.push_back(std::move(rule));
}

} // namespace

// A later "!" rule cannot bring back a file when a parent directory is already ignored.
[[nodiscard]] bool ignored(const GitIgnore &ignore, std::string_view relative, bool directory)
{
    if (!ignore.present || ignore.rules.empty() || relative.empty())
    {
        return false;
    }
    std::size_t start = 0;
    while (start < relative.size())
    {
        const auto slash = relative.find('/', start);
        if (slash == std::string_view::npos)
        {
            break;
        }
        if (direct_ignored(ignore.rules, relative.substr(0, slash), true))
        {
            return true;
        }
        start = slash + 1;
    }
    return direct_ignored(ignore.rules, relative, directory);
}

[[nodiscard]] GitIgnore load_gitignore(const std::filesystem::path &cwd)
{
    GitIgnore ignore;
    const auto path = cwd / ".gitignore";
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec))
    {
        return ignore;
    }
    ignore.present = true;
    const std::string text = read_text(path);
    std::size_t start = 0;
    while (start <= text.size())
    {
        const auto next = text.find('\n', start);
        const auto line =
            std::string_view(text).substr(start, next == std::string::npos ? std::string_view::npos : next - start);
        add_ignore_rule(ignore, line);
        if (next == std::string::npos)
        {
            break;
        }
        start = next + 1;
    }
    return ignore;
}

[[nodiscard]] std::string skip_note(const GitIgnore &ignore)
{
    std::string out = "Names starting with '.' are omitted.\n";
    if (!ignore.present)
    {
        out += "No .gitignore.\n";
        return out;
    }
    if (ignore.patterns.empty())
    {
        out += ".gitignore has no patterns.\n";
        return out;
    }
    out += ".gitignore: ";
    int shown = 0;
    for (const auto &pattern : ignore.patterns)
    {
        if (shown == kPatternCap)
        {
            out += ", …";
            break;
        }
        if (shown > 0)
        {
            out += ", ";
        }
        out += pattern;
        ++shown;
    }
    out.push_back('\n');
    return out;
}

} // namespace project_detail

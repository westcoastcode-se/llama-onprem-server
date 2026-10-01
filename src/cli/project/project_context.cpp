#include "cli/project/project_context.hpp"

#include "common/tools.hpp"

#include <algorithm>
#include <fstream>
#include <set>
#include <sstream>

namespace
{

[[nodiscard]] std::string read_text(const std::filesystem::path &path)
{
    std::ifstream in(path);
    if (!in)
    {
        return {};
    }
    std::stringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

[[nodiscard]] std::string trim_copy(std::string_view text)
{
    return std::string(string_view_trim(text));
}

[[nodiscard]] std::string one_line(std::string_view text, std::size_t cap)
{
    std::string out;
    for (const char c : text)
    {
        if (c == '\n' || c == '\r' || c == '\t')
        {
            if (!out.empty() && out.back() != ' ')
            {
                out.push_back(' ');
            }
            continue;
        }
        out.push_back(c);
        if (out.size() >= cap)
        {
            break;
        }
    }
    return trim_copy(out);
}

struct Front
{
    std::string name;
    std::string description;
    std::string_view body;
};

[[nodiscard]] Front read_front(std::string_view text)
{
    Front front;
    front.body = text;
    if (!text.starts_with("---"))
    {
        return front;
    }
    const auto end = text.find("\n---", 3);
    if (end == std::string_view::npos)
    {
        return front;
    }
    const auto matter = text.substr(0, end);
    std::size_t pos = 0;
    while (pos < matter.size())
    {
        auto next = matter.find('\n', pos);
        auto line = matter.substr(pos, next == std::string_view::npos ? std::string_view::npos : next - pos);
        line = string_view_trim(line);
        if (line.starts_with("name:"))
        {
            front.name = trim_copy(line.substr(5));
        }
        else if (line.starts_with("description:"))
        {
            front.description = trim_copy(line.substr(12));
        }
        if (next == std::string_view::npos)
        {
            break;
        }
        pos = next + 1;
    }
    auto body = text.substr(end + 4);
    if (body.starts_with("\n"))
    {
        body.remove_prefix(1);
    }
    front.body = body;
    return front;
}

[[nodiscard]] std::string first_summary(std::string_view body)
{
    std::size_t pos = 0;
    while (pos < body.size())
    {
        auto next = body.find('\n', pos);
        auto line =
            string_view_trim(body.substr(pos, next == std::string_view::npos ? std::string_view::npos : next - pos));
        if (!line.empty() && !line.starts_with('#'))
        {
            return one_line(line, 160);
        }
        if (next == std::string_view::npos)
        {
            break;
        }
        pos = next + 1;
    }
    return {};
}

} // namespace

std::vector<SkillNote> list_skills(const std::filesystem::path &cwd)
{
    std::vector<SkillNote> notes;
    const auto root = cwd / skills_directory();
    std::error_code ec;
    if (!std::filesystem::is_directory(root, ec))
    {
        return notes;
    }
    for (const auto &entry : std::filesystem::directory_iterator(root, ec))
    {
        if (ec || !entry.is_directory(ec))
        {
            continue;
        }
        const auto file = entry.path() / "SKILL.md";
        if (!std::filesystem::is_regular_file(file, ec))
        {
            continue;
        }
        const std::string text = read_text(file);
        const Front front = read_front(text);
        SkillNote note;
        note.name = front.name.empty() ? entry.path().filename().string() : front.name;
        note.summary = front.description.empty() ? first_summary(front.body) : one_line(front.description, 160);
        std::error_code rel_ec;
        const auto relative = std::filesystem::relative(file, cwd, rel_ec);
        note.path = rel_ec ? file.generic_string() : relative.generic_string();
        notes.push_back(std::move(note));
    }
    std::ranges::sort(notes, [](const SkillNote &left, const SkillNote &right) { return left.name < right.name; });
    return notes;
}

static void list_extensions(const std::filesystem::path &cwd, std::set<std::string> &extensions)
{
    std::error_code ec;
    for (const auto &entry : std::filesystem::directory_iterator(cwd, ec))
    {
        if (ec)
        {
            continue;
        }

        const auto name = entry.path().filename().string();
        if (name == "." || name == "..")
        {
            continue;
        }

        if (entry.is_directory())
        {
            list_extensions(entry.path(), extensions);
        }
        else
        {
            extensions.insert(entry.path().extension().string());
        }
    }
}

std::vector<std::string> list_extensions(const std::filesystem::path &cwd)
{
    std::set<std::string> extensions;
    list_extensions(cwd, extensions);
    std::vector result(extensions.begin(), extensions.end());
    std::ranges::sort(result, [](const std::string &left, const std::string &right) { return left < right; });
    return result;
}


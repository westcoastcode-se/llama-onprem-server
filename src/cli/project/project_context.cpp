#include "cli/project/project_context.hpp"

#include "cli/project/project_detail.hpp"

#include "common/tools.hpp"

#include <algorithm>
#include <set>

using namespace project_detail;

namespace
{

[[nodiscard]] std::string relative_path(const std::filesystem::path &root, const std::filesystem::path &path)
{
    std::error_code ec;
    const auto relative = std::filesystem::relative(path, root, ec);
    if (ec)
    {
        return path.filename().generic_string();
    }
    auto text = relative.generic_string();
    while (text.starts_with("./"))
    {
        text.erase(0, 2);
    }
    return text;
}

[[nodiscard]] std::string child_names(const std::filesystem::path &dir, const std::filesystem::path &root,
                                      const GitIgnore &ignore)
{
    std::vector<std::string> names;
    std::error_code ec;
    for (const auto &entry : std::filesystem::directory_iterator(dir, ec))
    {
        if (ec)
        {
            break;
        }
        const auto name = entry.path().filename().string();
        const bool directory = entry.is_directory(ec);
        if (name.empty() || name[0] == '.' || ignored(ignore, relative_path(root, entry.path()), directory))
        {
            continue;
        }
        names.push_back(directory ? name + "/" : name);
    }
    std::ranges::sort(names);
    std::string line;
    int shown = 0;
    for (const auto &name : names)
    {
        if (shown == kChildCap)
        {
            line += ", …";
            break;
        }
        if (!line.empty())
        {
            line += ", ";
        }
        line += name;
        ++shown;
    }
    return line;
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
    const auto root = cwd / get_intelligence_root_dir() / "skills";
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
    std::error_code ec;
    std::set<std::string> extensions;
    list_extensions(cwd, extensions);
    std::vector result(extensions.begin(), extensions.end());
    std::ranges::sort(result, [](const std::string &left, const std::string &right) { return left < right; });
    return result;
}

bool refresh_project_map(const std::filesystem::path &cwd, bool force)
{
    const auto dir = cwd / get_intelligence_root_dir();
    const auto map_path = dir / "map.md";
    const auto head_path = dir / "map.head";
    const std::string head = git_head(cwd);
    std::error_code ec;
    if (!force && std::filesystem::is_regular_file(map_path, ec) && std::filesystem::is_regular_file(head_path, ec) &&
        trim_copy(read_text(head_path)) == head)
    {
        return false;
    }

    const GitIgnore ignore = load_gitignore(cwd);
    std::string map = "# Project map\n\n";
    map += "Short index. Read this before searching an unfamiliar area. It is not the source.\n\n";
    map += "## Build and test\n";
    const std::string hints = build_hints(cwd);
    map += hints.empty() ? "- See the build files in the project root.\n" : hints;
    map += "\n## Layout\n";

    std::vector<std::filesystem::directory_entry> tops;
    for (const auto &entry : std::filesystem::directory_iterator(cwd, ec))
    {
        if (ec)
        {
            break;
        }
        const auto name = entry.path().filename().string();
        const bool directory = entry.is_directory(ec);
        if (name.empty() || name[0] == '.' || ignored(ignore, name, directory))
        {
            continue;
        }
        tops.push_back(entry);
    }
    std::ranges::sort(
        tops, [](const auto &left, const auto &right) { return left.path().filename() < right.path().filename(); });
    for (const auto &entry : tops)
    {
        const auto name = entry.path().filename().string();
        if (entry.is_directory(ec))
        {
            const auto children = child_names(entry.path(), cwd, ignore);
            map += "- " + name + "/";
            if (!children.empty())
            {
                map += ": " + children;
            }
            map += "\n";
        }
        else
        {
            map += "- " + name + "\n";
        }
    }
    map += "\n## Skip\n";
    map += skip_note(ignore);

    write_text(map_path, map);
    write_text(head_path, head);
    return true;
}


#include "cli/project_context.hpp"

#include "common/tools.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>

namespace
{

constexpr int kChildCap = 12;

const std::vector<std::string> kSkip = {
    ".git", "vendors", "cmake-build-debug", "cmake-build-release", "cmake-build-verify", "build", "node_modules",
    ".callisto",
};

[[nodiscard]] bool skipped(const std::string &name)
{
    return std::ranges::find(kSkip, name) != kSkip.end();
}

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

[[nodiscard]] std::filesystem::path git_dir(const std::filesystem::path &cwd)
{
    const auto dot = cwd / ".git";
    std::error_code ec;
    if (std::filesystem::is_regular_file(dot, ec))
    {
        const std::string text = read_text(dot);
        constexpr std::string_view kPrefix = "gitdir:";
        if (text.starts_with(kPrefix))
        {
            const auto gitdir = std::filesystem::path(trim_copy(std::string_view(text).substr(kPrefix.size())));
            return gitdir.is_absolute() ? gitdir : cwd / gitdir;
        }
    }
    if (std::filesystem::is_directory(dot, ec))
    {
        return dot;
    }
    return {};
}

[[nodiscard]] std::string git_head(const std::filesystem::path &cwd)
{
    const auto git = git_dir(cwd);
    if (git.empty())
    {
        return {};
    }
    const std::string head = trim_copy(read_text(git / "HEAD"));
    constexpr std::string_view kRef = "ref:";
    if (!head.starts_with(kRef))
    {
        return head;
    }
    const auto ref = trim_copy(std::string_view(head).substr(kRef.size()));
    const std::string sha = trim_copy(read_text(git / ref));
    if (!sha.empty())
    {
        return sha;
    }
    std::ifstream packed(git / "packed-refs");
    std::string line;
    while (std::getline(packed, line))
    {
        if (line.empty() || line[0] == '#' || line[0] == '^')
        {
            continue;
        }
        const auto space = line.find(' ');
        if (space != std::string::npos && trim_copy(std::string_view(line).substr(space + 1)) == ref)
        {
            return line.substr(0, space);
        }
    }
    return ref;
}

void write_text(const std::filesystem::path &path, std::string_view text)
{
    if (path.has_parent_path())
    {
        std::filesystem::create_directories(path.parent_path());
    }
    std::ofstream out(path, std::ios::binary);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
}

[[nodiscard]] std::string cmake_hints(const std::filesystem::path &cmake)
{
    std::string targets;
    std::string tests;
    std::ifstream in(cmake);
    std::string line;
    while (std::getline(in, line))
    {
        if (line.find("add_executable(") != std::string::npos)
        {
            const auto open = line.find('(');
            const auto name = string_view_trim(std::string_view(line).substr(open + 1));
            const auto end = name.find_first_of(" \t)");
            const auto target = end == std::string_view::npos ? name : name.substr(0, end);
            if (!target.empty())
            {
                if (!targets.empty())
                {
                    targets += ", ";
                }
                targets += target;
            }
        }
        else if (line.find("add_test(") != std::string::npos && line.find("NAME") != std::string::npos)
        {
            const auto name_at = line.find("NAME");
            auto rest = string_view_trim(std::string_view(line).substr(name_at + 4));
            const auto end = rest.find_first_of(" \t)");
            if (end != std::string_view::npos)
            {
                rest = rest.substr(0, end);
            }
            if (!rest.empty())
            {
                if (!tests.empty())
                {
                    tests += ", ";
                }
                tests += rest;
            }
        }
    }
    std::string out;
    if (!targets.empty())
    {
        out += "- Targets: " + targets + "\n";
    }
    if (!tests.empty())
    {
        out += "- Tests: " + tests + "\n";
    }
    return out;
}

[[nodiscard]] std::string child_names(const std::filesystem::path &dir)
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
        if (name.empty() || name[0] == '.' || skipped(name))
        {
            continue;
        }
        names.push_back(entry.is_directory(ec) ? name + "/" : name);
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
        auto line = string_view_trim(body.substr(pos, next == std::string_view::npos ? std::string_view::npos : next - pos));
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
    const auto root = cwd / ".callisto" / "skills";
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

bool refresh_project_map(const std::filesystem::path &cwd, bool force)
{
    const auto dir = cwd / ".callisto";
    const auto map_path = dir / "map.md";
    const auto head_path = dir / "map.head";
    const std::string head = git_head(cwd);
    std::error_code ec;
    if (!force && std::filesystem::is_regular_file(map_path, ec) && std::filesystem::is_regular_file(head_path, ec) &&
        trim_copy(read_text(head_path)) == head)
    {
        return false;
    }

    std::string map = "# Project map\n\n";
    map += "Short index. Read this before searching an unfamiliar area. It is not the source.\n\n";
    map += "## Build and test\n";
    const std::string hints = cmake_hints(cwd / "CMakeLists.txt");
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
        if (name.empty() || name[0] == '.' || skipped(name))
        {
            continue;
        }
        tops.push_back(entry);
    }
    std::ranges::sort(tops, [](const auto &left, const auto &right) {
        return left.path().filename() < right.path().filename();
    });
    for (const auto &entry : tops)
    {
        const auto name = entry.path().filename().string();
        if (entry.is_directory(ec))
        {
            const auto children = child_names(entry.path());
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
    map += "vendors, cmake-build-debug, cmake-build-release, build, node_modules, .git\n";

    write_text(map_path, map);
    write_text(head_path, head);
    return true;
}

#include "cli/project_context.hpp"

#include "common/tools.hpp"

#include <algorithm>
#include <fstream>
#include <set>
#include <sstream>

namespace
{

constexpr int kChildCap = 12;
constexpr int kPatternCap = 40;

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

[[nodiscard]] std::string cmake_section(const std::filesystem::path &cmake)
{
    std::error_code ec;
    if (!std::filesystem::is_regular_file(cmake, ec))
    {
        return {};
    }
    return "- Kind: CMake\n" + cmake_hints(cmake);
}

void append_list(std::string &out, std::string_view label, const std::vector<std::string> &names)
{
    if (names.empty())
    {
        return;
    }
    out += "- ";
    out += label;
    out += ": ";
    int shown = 0;
    for (const auto &name : names)
    {
        if (shown == kChildCap)
        {
            out += ", …";
            break;
        }
        if (shown > 0)
        {
            out += ", ";
        }
        out += name;
        ++shown;
    }
    out.push_back('\n');
}

[[nodiscard]] std::string node_section(const std::filesystem::path &manifest)
{
    std::error_code ec;
    if (!std::filesystem::is_regular_file(manifest, ec))
    {
        return {};
    }
    std::string out = "- Kind: Node.js\n";
    const auto doc = nlohmann::json::parse(read_text(manifest), nullptr, false);
    if (doc.is_discarded() || !doc.is_object())
    {
        return out;
    }
    if (doc.contains("name") && doc["name"].is_string())
    {
        const auto name = doc["name"].get<std::string>();
        if (!name.empty())
        {
            out += "- Package: " + name + "\n";
        }
    }
    std::vector<std::string> scripts;
    if (doc.contains("scripts") && doc["scripts"].is_object())
    {
        for (const auto &item : doc["scripts"].items())
        {
            scripts.push_back(item.key());
        }
    }
    std::ranges::sort(scripts);
    append_list(out, "Scripts", scripts);
    if (std::ranges::find(scripts, "test") != scripts.end())
    {
        out += "- Tests: npm test\n";
    }
    return out;
}

[[nodiscard]] std::string toml_scalar(std::string_view value)
{
    value = string_view_trim(value);
    if (value.size() >= 2 && (value.front() == '"' || value.front() == '\''))
    {
        const char quote = value.front();
        const auto end = value.find(quote, 1);
        if (end != std::string_view::npos)
        {
            return std::string(value.substr(1, end - 1));
        }
    }
    const auto hash = value.find('#');
    if (hash != std::string_view::npos)
    {
        value = string_view_trim(value.substr(0, hash));
    }
    return std::string(value);
}

[[nodiscard]] std::vector<std::string> quoted_strings(std::string_view text)
{
    std::vector<std::string> out;
    for (std::size_t i = 0; i < text.size(); ++i)
    {
        if (text[i] != '"')
        {
            continue;
        }
        const auto end = text.find('"', i + 1);
        if (end == std::string_view::npos)
        {
            break;
        }
        out.emplace_back(text.substr(i + 1, end - i - 1));
        i = end;
    }
    return out;
}

[[nodiscard]] std::string rust_section(const std::filesystem::path &manifest)
{
    std::error_code ec;
    if (!std::filesystem::is_regular_file(manifest, ec))
    {
        return {};
    }
    enum class Section
    {
        None,
        Package,
        Workspace,
        Bin,
        Other
    };
    Section section = Section::None;
    bool reading_array = false;
    std::string array_text;
    std::string package;
    std::vector<std::string> bins;
    std::vector<std::string> members;
    std::ifstream in(manifest);
    std::string line;
    auto finish_array = [&]() {
        if (section == Section::Workspace)
        {
            for (auto &name : quoted_strings(array_text))
            {
                members.push_back(std::move(name));
            }
        }
        reading_array = false;
        array_text.clear();
    };
    while (std::getline(in, line))
    {
        const auto text = string_view_trim(std::string_view(line));
        if (text.empty() || text.starts_with('#'))
        {
            continue;
        }
        if (reading_array)
        {
            array_text.append(text);
            if (text.find(']') != std::string_view::npos)
            {
                finish_array();
            }
            continue;
        }
        if (text.starts_with('['))
        {
            auto header = text;
            const auto comment = header.find('#');
            if (comment != std::string_view::npos)
            {
                header = string_view_trim(header.substr(0, comment));
            }
            if (header == "[package]")
            {
                section = Section::Package;
            }
            else if (header == "[workspace]")
            {
                section = Section::Workspace;
            }
            else if (header == "[[bin]]")
            {
                section = Section::Bin;
            }
            else
            {
                section = Section::Other;
            }
            continue;
        }
        if (section == Section::None || section == Section::Other)
        {
            continue;
        }
        const auto eq = text.find('=');
        if (eq == std::string_view::npos)
        {
            continue;
        }
        const auto key = string_view_trim(text.substr(0, eq));
        const auto value = string_view_trim(text.substr(eq + 1));
        if (key == "members" && section == Section::Workspace && value.find('[') != std::string_view::npos)
        {
            array_text = std::string(value);
            if (value.find(']') == std::string_view::npos)
            {
                reading_array = true;
            }
            else
            {
                finish_array();
            }
            continue;
        }
        if (key != "name")
        {
            continue;
        }
        const auto name = toml_scalar(value);
        if (name.empty())
        {
            continue;
        }
        if (section == Section::Package && package.empty())
        {
            package = name;
        }
        else if (section == Section::Bin)
        {
            bins.push_back(name);
        }
    }
    if (reading_array)
    {
        finish_array();
    }
    std::string out = "- Kind: Rust\n";
    if (!package.empty())
    {
        out += "- Package: " + package + "\n";
    }
    append_list(out, "Bins", bins);
    append_list(out, "Workspace", members);
    out += "- Tests: cargo test\n";
    return out;
}

[[nodiscard]] bool xml_name_char(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-' ||
           c == '.';
}

[[nodiscard]] bool maven_skip_tag(std::string_view name)
{
    return name == "parent" || name == "dependencies" || name == "dependency" || name == "dependencyManagement" ||
           name == "plugin" || name == "plugins" || name == "pluginManagement" || name == "profile" ||
           name == "profiles" || name == "reporting" || name == "extensions";
}

[[nodiscard]] std::string maven_section(const std::filesystem::path &manifest)
{
    std::error_code ec;
    if (!std::filesystem::is_regular_file(manifest, ec))
    {
        return {};
    }
    const std::string text = read_text(manifest);
    int skip = 0;
    int modules = 0;
    std::string artifact;
    std::vector<std::string> module_names;
    std::size_t pos = 0;
    while (pos < text.size())
    {
        const auto open = text.find('<', pos);
        if (open == std::string::npos)
        {
            break;
        }
        if (text.compare(open, 4, "<!--") == 0)
        {
            const auto end = text.find("-->", open + 4);
            pos = end == std::string::npos ? text.size() : end + 3;
            continue;
        }
        if (open + 1 < text.size() && (text[open + 1] == '?' || text[open + 1] == '!'))
        {
            const auto end = text.find('>', open + 2);
            pos = end == std::string::npos ? text.size() : end + 1;
            continue;
        }
        const bool closing = open + 1 < text.size() && text[open + 1] == '/';
        auto name_at = open + (closing ? 2 : 1);
        auto name_end = name_at;
        while (name_end < text.size() && xml_name_char(text[name_end]))
        {
            ++name_end;
        }
        if (name_end == name_at)
        {
            pos = open + 1;
            continue;
        }
        auto name = std::string_view(text).substr(name_at, name_end - name_at);
        const auto colon = name.rfind(':');
        if (colon != std::string_view::npos)
        {
            name.remove_prefix(colon + 1);
        }
        const auto gt = text.find('>', name_end);
        if (gt == std::string::npos)
        {
            break;
        }
        const bool self_closing = gt > name_end && text[gt - 1] == '/';
        pos = gt + 1;
        if (self_closing)
        {
            continue;
        }
        if (maven_skip_tag(name))
        {
            if (closing)
            {
                skip = std::max(0, skip - 1);
            }
            else
            {
                ++skip;
            }
            continue;
        }
        if (name == "modules")
        {
            if (closing)
            {
                modules = std::max(0, modules - 1);
            }
            else
            {
                ++modules;
            }
            continue;
        }
        if (skip > 0 || closing)
        {
            continue;
        }
        const auto content_end = text.find('<', pos);
        const auto content = trim_copy(std::string_view(text).substr(
            pos, content_end == std::string::npos ? std::string_view::npos : content_end - pos));
        if (content.empty())
        {
            continue;
        }
        if (name == "artifactId" && artifact.empty())
        {
            artifact = content;
        }
        else if (name == "module" && modules > 0)
        {
            module_names.push_back(content);
        }
    }
    std::string out = "- Kind: Maven\n";
    if (!artifact.empty())
    {
        out += "- Artifact: " + artifact + "\n";
    }
    append_list(out, "Modules", module_names);
    out += "- Tests: mvn test\n";
    return out;
}

[[nodiscard]] std::string build_hints(const std::filesystem::path &cwd)
{
    std::string out;
    const auto append = [&](std::string part) {
        if (part.empty())
        {
            return;
        }
        if (!out.empty())
        {
            out.push_back('\n');
        }
        out += part;
    };
    append(cmake_section(cwd / "CMakeLists.txt"));
    append(node_section(cwd / "package.json"));
    append(rust_section(cwd / "Cargo.toml"));
    append(maven_section(cwd / "pom.xml"));
    return out;
}

struct IgnoreRule
{
    std::vector<std::string> parts;
    std::string base;
    bool negate = false;
    bool directory_only = false;
    bool anchored = false;
};

struct GitIgnore
{
    bool present = false;
    std::vector<IgnoreRule> rules;
    std::vector<std::string> patterns;
};

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

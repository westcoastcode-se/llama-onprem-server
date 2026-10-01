#include "cli/project/project_detail.hpp"

#include <algorithm>
#include <fstream>
#include <nlohmann/json.hpp>

namespace project_detail
{
namespace
{

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

} // namespace

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

} // namespace project_detail

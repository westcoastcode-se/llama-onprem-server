#include "cli/project/project_detail.hpp"

#include <fstream>

namespace project_detail
{
namespace
{

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

} // namespace

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

} // namespace project_detail

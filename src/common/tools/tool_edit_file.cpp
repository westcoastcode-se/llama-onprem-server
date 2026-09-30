#include "common/tools/tool_edit_file.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace Tools {
namespace {

struct FileText {
    std::vector<std::string> lines;
    bool trailing_newline = false;
};

struct PatchLine {
    char kind = ' ';
    std::string text;
};

struct Hunk {
    int number = 0;
    int old_start = 0;
    int old_count = 1;
    bool pure_insert = false;
    std::vector<PatchLine> lines;
};

struct Span {
    int from = 0;
    int old_len = 0;
    std::vector<std::string> neu;
};

FileText split_lines(const std::string &content)
{
    FileText out;
    if (content.empty())
    {
        return out;
    }
    out.trailing_newline = content.back() == '\n';
    std::size_t at = 0;
    while (at < content.size())
    {
        const std::size_t nl = content.find('\n', at);
        if (nl == std::string::npos)
        {
            out.lines.push_back(content.substr(at));
            break;
        }
        out.lines.push_back(content.substr(at, nl - at));
        at = nl + 1;
        if (at == content.size())
        {
            break;
        }
    }
    return out;
}

std::string join_lines(const std::vector<std::string> &lines, bool trailing_newline)
{
    std::string out;
    for (std::size_t i = 0; i < lines.size(); ++i)
    {
        if (i != 0)
        {
            out += '\n';
        }
        out += lines[i];
    }
    if (trailing_newline && !lines.empty())
    {
        out += '\n';
    }
    return out;
}

std::vector<std::string> patch_lines(const std::string &patch)
{
    std::vector<std::string> lines;
    std::size_t at = 0;
    while (at < patch.size())
    {
        const std::size_t nl = patch.find('\n', at);
        std::string line = patch.substr(at, nl == std::string::npos ? std::string::npos : nl - at);
        if (!line.empty() && line.back() == '\r')
        {
            line.pop_back();
        }
        lines.push_back(std::move(line));
        if (nl == std::string::npos)
        {
            break;
        }
        at = nl + 1;
    }
    return lines;
}

bool read_int(std::string_view text, std::size_t &i, int &value)
{
    if (i >= text.size() || std::isdigit(static_cast<unsigned char>(text[i])) == 0)
    {
        return false;
    }
    int parsed = 0;
    while (i < text.size() && std::isdigit(static_cast<unsigned char>(text[i])) != 0)
    {
        parsed = parsed * 10 + (text[i] - '0');
        ++i;
    }
    value = parsed;
    return true;
}

bool parse_hunk_header(std::string_view line, int &old_start, int &old_count)
{
    if (!line.starts_with("@@"))
    {
        return false;
    }
    std::size_t i = 2;
    while (i < line.size() && line[i] == ' ')
    {
        ++i;
    }
    if (i >= line.size() || line[i] != '-')
    {
        return false;
    }
    ++i;
    if (!read_int(line, i, old_start))
    {
        return false;
    }
    old_count = 1;
    if (i < line.size() && line[i] == ',')
    {
        ++i;
        if (!read_int(line, i, old_count))
        {
            return false;
        }
    }
    if (i >= line.size() || line[i] != ' ' || i + 1 >= line.size() || line[i + 1] != '+')
    {
        return false;
    }
    i += 2;
    int new_start = 0;
    if (!read_int(line, i, new_start))
    {
        return false;
    }
    if (i < line.size() && line[i] == ',')
    {
        ++i;
        int new_count = 0;
        if (!read_int(line, i, new_count))
        {
            return false;
        }
    }
    return true;
}

std::string header_path(std::string_view line)
{
    if (line.starts_with("--- ") || line.starts_with("+++ "))
    {
        line.remove_prefix(4);
    }
    const std::size_t tab = line.find('\t');
    if (tab != std::string_view::npos)
    {
        line = line.substr(0, tab);
    }
    if (line.size() >= 2 && line.front() == '"' && line.back() == '"')
    {
        line.remove_prefix(1);
        line.remove_suffix(1);
    }
    return std::string(line);
}

bool same_target(const std::string &file, std::string header)
{
    if (header.starts_with("a/") || header.starts_with("b/"))
    {
        header.erase(0, 2);
    }
    if (header.empty() || header == "/dev/null")
    {
        return false;
    }
    if (file == header || file.ends_with("/" + header))
    {
        return true;
    }
    return header.find('/') == std::string::npos && std::filesystem::path(file).filename() == header;
}

bool preamble_line(std::string_view line)
{
    return line.starts_with("diff --git ") || line.starts_with("index ") || line.starts_with("new file ") ||
           line.starts_with("deleted file ") || line.starts_with("similarity ") || line.starts_with("rename ") ||
           line.starts_with("old mode ") || line.starts_with("new mode ") || line.starts_with("Binary files ");
}

std::string parse_patch(const std::string &patch, const std::string &path, std::vector<Hunk> &hunks, bool &new_file)
{
    new_file = false;
    const std::vector<std::string> lines = patch_lines(patch);
    bool in_hunk = false;
    bool saw_old_null = false;
    bool saw_new_null = false;
    int git_headers = 0;
    int new_headers = 0;
    std::string plus_path;
    Hunk current;

    auto close_hunk = [&]() -> std::string {
        if (!in_hunk)
        {
            return {};
        }
        in_hunk = false;
        if (current.lines.empty())
        {
            return "error: hunk " + std::to_string(current.number) + " is empty";
        }
        hunks.push_back(std::move(current));
        current = {};
        return {};
    };

    for (const std::string &line : lines)
    {
        if (line.starts_with("@@"))
        {
            if (const std::string closed = close_hunk(); !closed.empty())
            {
                return closed;
            }
            int old_start = 0;
            int old_count = 1;
            if (!parse_hunk_header(line, old_start, old_count))
            {
                return "error: malformed hunk header: " + line;
            }
            current.number = static_cast<int>(hunks.size()) + 1;
            current.old_start = old_start;
            current.old_count = old_count;
            current.pure_insert = old_count == 0;
            in_hunk = true;
            continue;
        }
        if (in_hunk && (line.starts_with("diff --git ") || line.starts_with("--- ") || line.starts_with("+++ ")))
        {
            if (const std::string closed = close_hunk(); !closed.empty())
            {
                return closed;
            }
        }
        if (in_hunk)
        {
            if (line.empty() || line.starts_with("\\"))
            {
                continue;
            }
            const char kind = line.front();
            if (kind != ' ' && kind != '+' && kind != '-')
            {
                const std::string shown = line.size() > 80 ? line.substr(0, 80) : line;
                return "error: hunk " + std::to_string(current.number) +
                       " has a line that does not start with a space, + or -: " + shown;
            }
            current.lines.push_back(PatchLine{kind, line.substr(1)});
            continue;
        }
        if (line.starts_with("diff --git ") && ++git_headers > 1)
        {
            return "error: a patch must change one file";
        }
        if (line.starts_with("--- "))
        {
            if (header_path(line) == "/dev/null")
            {
                saw_old_null = true;
            }
            continue;
        }
        if (line.starts_with("+++ "))
        {
            if (++new_headers > 1)
            {
                return "error: a patch must change one file";
            }
            plus_path = header_path(line);
            if (plus_path == "/dev/null")
            {
                saw_new_null = true;
            }
            continue;
        }
        if (line.empty() || preamble_line(line))
        {
            continue;
        }
    }
    if (const std::string closed = close_hunk(); !closed.empty())
    {
        return closed;
    }
    if (hunks.empty())
    {
        return "error: patch has no hunks";
    }
    new_file = saw_old_null;
    if (saw_new_null)
    {
        return "error: patch deletes a file; edit_file only changes lines";
    }
    if (!plus_path.empty() && !same_target(path, plus_path))
    {
        return "error: patch is for " + plus_path + ", not " + path;
    }
    return {};
}

std::vector<std::string> old_side(const Hunk &hunk)
{
    std::vector<std::string> lines;
    for (const PatchLine &line : hunk.lines)
    {
        if (line.kind == ' ' || line.kind == '-')
        {
            lines.push_back(line.text);
        }
    }
    return lines;
}

std::vector<std::string> new_side(const Hunk &hunk)
{
    std::vector<std::string> lines;
    for (const PatchLine &line : hunk.lines)
    {
        if (line.kind == ' ' || line.kind == '+')
        {
            lines.push_back(line.text);
        }
    }
    return lines;
}

bool lines_match(const std::vector<std::string> &file, int at, const std::vector<std::string> &old)
{
    if (at < 0 || static_cast<std::size_t>(at) + old.size() > file.size())
    {
        return false;
    }
    for (std::size_t i = 0; i < old.size(); ++i)
    {
        if (file[static_cast<std::size_t>(at) + i] != old[i])
        {
            return false;
        }
    }
    return true;
}

int locate_hunk(const std::vector<std::string> &file, const std::vector<std::string> &old, int hint, bool &ambiguous)
{
    ambiguous = false;
    int best = -1;
    long best_distance = 0;
    int ties = 0;
    const int last = static_cast<int>(file.size()) - static_cast<int>(old.size());
    for (int at = 0; at <= last; ++at)
    {
        if (!lines_match(file, at, old))
        {
            continue;
        }
        const long distance = std::labs(static_cast<long>(at) - hint);
        if (best < 0 || distance < best_distance)
        {
            best = at;
            best_distance = distance;
            ties = 1;
        }
        else if (distance == best_distance)
        {
            ++ties;
        }
    }
    if (ties > 1)
    {
        ambiguous = true;
        return -1;
    }
    return best;
}

bool ranges_overlap(int left, int left_len, int right, int right_len)
{
    if (left_len == 0 && right_len == 0)
    {
        return left == right;
    }
    if (left_len == 0)
    {
        return right <= left && left < right + right_len;
    }
    if (right_len == 0)
    {
        return left <= right && right < left + left_len;
    }
    return left < right + right_len && right < left + left_len;
}

bool additions_only(const std::vector<Hunk> &hunks)
{
    if (hunks.empty())
    {
        return false;
    }
    for (const Hunk &hunk : hunks)
    {
        for (const PatchLine &line : hunk.lines)
        {
            if (line.kind != '+')
            {
                return false;
            }
        }
    }
    return true;
}

std::string format_diff(const std::string &path, const std::vector<std::string> &old_lines, std::vector<Span> spans,
                        bool created)
{
    std::ranges::sort(spans, [](const Span &left, const Span &right) { return left.from < right.from; });
    std::string out = created ? "--- /dev/null\n+++ " + path + "\n" : "--- " + path + "\n+++ " + path + "\n";
    for (std::size_t s = 0; s < spans.size(); ++s)
    {
        const int from = spans[s].from;
        const int to = from + spans[s].old_len;
        const int prev_end = s == 0 ? 0 : spans[s - 1].from + spans[s - 1].old_len;
        const int next_from =
            s + 1 == spans.size() ? static_cast<int>(old_lines.size()) : spans[s + 1].from;
        const int ctx_lo = std::max(prev_end, from - 3);
        const int ctx_hi = std::min(next_from, to + 3);
        const int old_count = ctx_hi - ctx_lo;
        const int new_count =
            (from - ctx_lo) + static_cast<int>(spans[s].neu.size()) + (ctx_hi - to);
        const int old_start = old_count == 0 ? from : ctx_lo + 1;
        const int new_start = new_count == 0 ? from : ctx_lo + 1;
        out += "@@ -" + std::to_string(old_start) + "," + std::to_string(old_count) + " +" +
               std::to_string(new_start) + "," + std::to_string(new_count) + " @@\n";
        auto emit = [&](char mark, const std::string &line) {
            out += mark;
            out += line;
            out += '\n';
        };
        for (int i = ctx_lo; i < from; ++i)
        {
            emit(' ', old_lines[static_cast<std::size_t>(i)]);
        }
        for (int i = from; i < to; ++i)
        {
            emit('-', old_lines[static_cast<std::size_t>(i)]);
        }
        for (const std::string &line : spans[s].neu)
        {
            emit('+', line);
        }
        for (int i = to; i < ctx_hi; ++i)
        {
            emit(' ', old_lines[static_cast<std::size_t>(i)]);
        }
    }
    if (out.size() > MAX_TOOL_OUTPUT_CHARS)
    {
        out.resize(MAX_TOOL_OUTPUT_CHARS);
        out += "\n... [diff truncated]";
    }
    return out;
}

bool write_atomic(const std::filesystem::path &path, const std::string &content, std::string &error)
{
    const std::filesystem::path tmp = path.string() + ".callisto-tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out.is_open())
        {
            error = "error: failed to open file for writing: " + path.string();
            return false;
        }
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
        out.flush();
        if (!out)
        {
            out.close();
            std::filesystem::remove(tmp);
            error = "error: failed to write file: " + path.string();
            return false;
        }
    }
    std::filesystem::rename(tmp, path);
    return true;
}

} // namespace

std::string edit_file(const nlohmann::json &args)
{
    if (!args.contains("path") || !args["path"].is_string())
    {
        return "error: missing required string argument 'path'";
    }
    if (!args.contains("patch") || !args["patch"].is_string())
    {
        return "error: missing required string argument 'patch'";
    }

    const std::string path_text = args["path"].get<std::string>();
    std::vector<Hunk> hunks;
    bool new_file = false;
    if (const std::string parsed = parse_patch(args["patch"].get<std::string>(), path_text, hunks, new_file);
        !parsed.empty())
    {
        return parsed;
    }

    std::ifstream in(path_text);
    const bool missing = !in.is_open();
    std::string original;
    if (!missing)
    {
        original.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        if (in.bad())
        {
            return "error: failed to read file '" + path_text + "'";
        }
        if (new_file)
        {
            return "error: " + path_text + " already exists; match its current lines";
        }
    }
    else if (!additions_only(hunks))
    {
        return "error: " + path_text + " does not exist; a new file patch contains only + lines";
    }
    else
    {
        std::vector<std::string> lines;
        for (const Hunk &hunk : hunks)
        {
            for (const PatchLine &line : hunk.lines)
            {
                lines.push_back(line.text);
            }
        }
        const std::string body = join_lines(lines, !lines.empty());
        std::string error;
        try
        {
            const std::filesystem::path file_path(path_text);
            if (file_path.has_parent_path())
            {
                std::filesystem::create_directories(file_path.parent_path());
            }
            if (!write_atomic(file_path, body, error))
            {
                return error;
            }
        }
        catch (const std::exception &ex)
        {
            return std::string("error writing file: ") + ex.what();
        }
        Span created;
        created.neu = std::move(lines);
        return format_diff(path_text, {}, {std::move(created)}, true);
    }
    const FileText file = split_lines(original);

    std::vector<Span> spans;
    spans.reserve(hunks.size());
    for (const Hunk &hunk : hunks)
    {
        const std::vector<std::string> old_lines = old_side(hunk);
        int lead = 0;
        while (lead < static_cast<int>(hunk.lines.size()) && hunk.lines[static_cast<std::size_t>(lead)].kind == ' ')
        {
            ++lead;
        }
        int tail = static_cast<int>(hunk.lines.size());
        while (tail > lead && hunk.lines[static_cast<std::size_t>(tail - 1)].kind == ' ')
        {
            --tail;
        }
        Span span;
        for (int i = lead; i < tail; ++i)
        {
            const PatchLine &line = hunk.lines[static_cast<std::size_t>(i)];
            if (line.kind == ' ' || line.kind == '-')
            {
                ++span.old_len;
            }
            if (line.kind == ' ' || line.kind == '+')
            {
                span.neu.push_back(line.text);
            }
        }
        if (old_lines.empty())
        {
            if (!hunk.pure_insert)
            {
                return "error: hunk " + std::to_string(hunk.number) +
                       " has no context; include a few unchanged lines around the change";
            }
            if (hunk.old_start < 0 || hunk.old_start > static_cast<int>(file.lines.size()))
            {
                return "error: hunk " + std::to_string(hunk.number) + " inserts at line " +
                       std::to_string(hunk.old_start) + ", past the end of " + path_text + " (" +
                       std::to_string(file.lines.size()) + " lines)";
            }
            span.from = hunk.old_start;
            span.old_len = 0;
            span.neu = new_side(hunk);
        }
        else
        {
            bool ambiguous = false;
            const int at = locate_hunk(file.lines, old_lines, hunk.old_start - 1, ambiguous);
            if (ambiguous)
            {
                return "error: hunk " + std::to_string(hunk.number) + " matches more than one place in " + path_text +
                       "; add a few unchanged lines";
            }
            if (at < 0)
            {
                return "error: hunk " + std::to_string(hunk.number) + " did not match " + path_text;
            }
            span.from = at + lead;
        }
        if (span.old_len == 0 && span.neu.empty())
        {
            continue;
        }
        for (const Span &earlier : spans)
        {
            if (ranges_overlap(earlier.from, earlier.old_len, span.from, span.old_len))
            {
                return "error: hunk " + std::to_string(hunk.number) + " overlaps an earlier hunk";
            }
        }
        spans.push_back(std::move(span));
    }

    std::vector<std::string> updated = file.lines;
    std::vector<Span> ordered = spans;
    std::ranges::sort(ordered, [](const Span &left, const Span &right) { return left.from > right.from; });
    for (const Span &span : ordered)
    {
        const auto begin = updated.begin() + span.from;
        updated.erase(begin, begin + span.old_len);
        updated.insert(updated.begin() + span.from, span.neu.begin(), span.neu.end());
    }

    bool trailing_newline = file.trailing_newline;
    if (original.empty() && !updated.empty())
    {
        trailing_newline = true;
    }
    const std::string body = join_lines(updated, trailing_newline);
    if (body == original)
    {
        return "error: patch does not change the file";
    }

    std::string error;
    try
    {
        if (!write_atomic(path_text, body, error))
        {
            return error;
        }
    }
    catch (const std::exception &ex)
    {
        return std::string("error writing file: ") + ex.what();
    }
    return format_diff(path_text, file.lines, std::move(spans), false);
}

Tool create_edit_file_tool()
{
    return {
        .name = std::string(kEditFileName),
        .description = std::string(kEditFileDescription),
        .schema_doc = std::string(kEditFileSchema),
        .execute = edit_file,
        .present = [](const nlohmann::json &args) { return tool_arg(args, "path"); }};
}

} // namespace Tools

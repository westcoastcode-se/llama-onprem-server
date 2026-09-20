#include "../agent/response_parse.hpp"
#include <cctype>
#include <sstream>

namespace
{

std::string_view trim_sv(std::string_view text)
{
    constexpr std::string_view ws = " \t\r\n";
    const auto b = text.find_first_not_of(ws);
    if (b == std::string_view::npos)
    {
        return {};
    }
    const auto e = text.find_last_not_of(ws);
    return text.substr(b, e - b + 1);
}

bool ieq(std::string_view a, std::string_view b)
{
    if (a.size() != b.size())
    {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i)
    {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i])))
        {
            return false;
        }
    }
    return true;
}

bool extract_tag_body(std::string_view text, std::string_view open_name, size_t open_gt,
                      std::string_view &body, size_t &after_close)
{
    // open_gt points at '>' of opening tag
    const std::string close = std::string("</") + std::string(open_name) + ">";
    size_t depth = 1;
    size_t pos = open_gt + 1;
    const size_t body_start = pos;

    while (pos < text.size())
    {
        const size_t lt = text.find('<', pos);
        if (lt == std::string_view::npos)
        {
            body = text.substr(body_start);
            after_close = text.size();
            return false; // unclosed
        }

        // closing tag?
        if (lt + 1 < text.size() && text[lt + 1] == '/')
        {
            const size_t gt = text.find('>', lt);
            if (gt == std::string_view::npos)
            {
                break;
            }
            auto name = trim_sv(text.substr(lt + 2, gt - lt - 2));
            if (ieq(name, open_name))
            {
                --depth;
                if (depth == 0)
                {
                    body = text.substr(body_start, lt - body_start);
                    after_close = gt + 1;
                    return true;
                }
            }
            pos = gt + 1;
            continue;
        }

        // nested same open tag
        const size_t gt = text.find('>', lt);
        if (gt == std::string_view::npos)
        {
            break;
        }
        auto raw = text.substr(lt + 1, gt - lt - 1);
        if (!raw.empty() && raw.back() == '/')
        {
            raw.remove_suffix(1);
        }
        auto name = trim_sv(raw);
        // drop attributes
        const auto sp = name.find_first_of(" \t");
        if (sp != std::string_view::npos)
        {
            name = name.substr(0, sp);
        }
        if (ieq(name, open_name))
        {
            ++depth;
        }
        pos = gt + 1;
    }

    body = text.substr(body_start);
    after_close = text.size();
    return false;
}

bool extract_single_tool(const nlohmann::json &j, ParsedToolCall &tc)
{
    if (!j.is_object())
    {
        return false;
    }
    if (j.contains("name") && j["name"].is_string())
    {
        tc.name = j["name"].get<std::string>();
    }
    else if (j.contains("tool") && j["tool"].is_string())
    {
        tc.name = j["tool"].get<std::string>();
    }
    else
    {
        return false;
    }

    if (j.contains("arguments"))
    {
        tc.arguments = j["arguments"];
    }
    else if (j.contains("args"))
    {
        tc.arguments = j["args"];
    }
    else if (j.contains("parameters"))
    {
        tc.arguments = j["parameters"];
    }
    else
    {
        tc.arguments = nlohmann::json::object();
        // leftover keys as arguments
        for (auto it = j.begin(); it != j.end(); ++it)
        {
            if (it.key() == "name" || it.key() == "tool" || it.key() == "id")
            {
                continue;
            }
            tc.arguments[it.key()] = it.value();
        }
    }
    if (j.contains("id") && j["id"].is_string())
    {
        tc.id = j["id"].get<std::string>();
    }
    return !tc.name.empty();
}

void parse_tool_json_segment(std::string_view segment, std::vector<ParsedToolCall> &out)
{
    auto trimmed = trim_sv(segment);
    if (trimmed.empty())
    {
        return;
    }

    // strip markdown fences
    if (trimmed.starts_with("```"))
    {
        auto nl = trimmed.find('\n');
        if (nl != std::string_view::npos)
        {
            trimmed.remove_prefix(nl + 1);
        }
        if (trimmed.ends_with("```"))
        {
            trimmed.remove_suffix(3);
        }
        trimmed = trim_sv(trimmed);
    }

    try
    {
        auto j = nlohmann::json::parse(trimmed);
        if (j.is_array())
        {
            for (const auto &el : j)
            {
                ParsedToolCall tc;
                if (extract_single_tool(el, tc))
                {
                    out.push_back(std::move(tc));
                }
            }
            return;
        }
        ParsedToolCall tc;
        if (extract_single_tool(j, tc))
        {
            out.push_back(std::move(tc));
        }
    }
    catch (...)
    {
        // ignore malformed
    }
}

std::string strip_special_blocks(std::string_view text)
{
    std::string out;
    out.reserve(text.size());
    size_t pos = 0;
    while (pos < text.size())
    {
        const size_t lt = text.find('<', pos);
        if (lt == std::string_view::npos)
        {
            out.append(text.substr(pos));
            break;
        }
        out.append(text.substr(pos, lt - pos));
        const size_t gt = text.find('>', lt);
        if (gt == std::string_view::npos)
        {
            out.append(text.substr(lt));
            break;
        }
        auto raw = text.substr(lt + 1, gt - lt - 1);
        bool closing = false;
        if (!raw.empty() && raw.front() == '/')
        {
            closing = true;
            raw.remove_prefix(1);
        }
        if (!raw.empty() && raw.back() == '/')
        {
            raw.remove_suffix(1);
        }
        auto name = trim_sv(raw);
        const auto sp = name.find_first_of(" \t");
        if (sp != std::string_view::npos)
        {
            name = name.substr(0, sp);
        }

        const bool special =
            ieq(name, "think") || ieq(name, "thinking") || ieq(name, "thought") ||
            ieq(name, "reasoning") || ieq(name, "tool_call") || ieq(name, "tool_calls") ||
            ieq(name, "question") || ieq(name, "answer") || ieq(name, "tool_response");

        if (!special)
        {
            out.append(text.substr(lt, gt - lt + 1));
            pos = gt + 1;
            continue;
        }

        if (closing || (gt > lt + 1 && text[gt - 1] == '/'))
        {
            pos = gt + 1;
            continue;
        }

        std::string_view body;
        size_t after = gt + 1;
        extract_tag_body(text, name, gt, body, after);
        pos = after;
    }
    return std::string(trim_sv(out));
}

} // namespace

ParsedAssistantActions parse_assistant_actions(std::string_view text)
{
    ParsedAssistantActions actions;
    if (text.empty())
    {
        return actions;
    }

    size_t pos = 0;
    while (pos < text.size())
    {
        const size_t lt = text.find('<', pos);
        if (lt == std::string_view::npos)
        {
            break;
        }
        const size_t gt = text.find('>', lt);
        if (gt == std::string_view::npos)
        {
            break;
        }

        auto raw = text.substr(lt + 1, gt - lt - 1);
        if (!raw.empty() && raw.front() == '/')
        {
            pos = gt + 1;
            continue;
        }
        bool self_close = false;
        if (!raw.empty() && raw.back() == '/')
        {
            self_close = true;
            raw.remove_suffix(1);
        }
        auto name = trim_sv(raw);
        const auto sp = name.find_first_of(" \t");
        if (sp != std::string_view::npos)
        {
            name = name.substr(0, sp);
        }

        if (self_close)
        {
            pos = gt + 1;
            continue;
        }

        if (ieq(name, "tool_call") || ieq(name, "tool_calls"))
        {
            std::string_view body;
            size_t after = gt + 1;
            extract_tag_body(text, name, gt, body, after);
            parse_tool_json_segment(body, actions.tool_calls);
            pos = after;
            continue;
        }

        if (ieq(name, "question"))
        {
            std::string_view body;
            size_t after = gt + 1;
            extract_tag_body(text, name, gt, body, after);
            if (!actions.question)
            {
                actions.question = ParsedQuestion{};
            }
            auto t = trim_sv(body);
            if (!t.empty())
            {
                if (!actions.question->text.empty())
                {
                    actions.question->text.push_back('\n');
                }
                actions.question->text.append(t);
            }
            pos = after;
            continue;
        }

        if (ieq(name, "answer"))
        {
            std::string_view body;
            size_t after = gt + 1;
            extract_tag_body(text, name, gt, body, after);
            if (!actions.question)
            {
                actions.question = ParsedQuestion{};
            }
            auto t = std::string(trim_sv(body));
            if (!t.empty())
            {
                actions.question->answers.push_back(std::move(t));
            }
            pos = after;
            continue;
        }

        pos = gt + 1;
    }

    // Fallback: whole text is a tool JSON object
    if (actions.tool_calls.empty())
    {
        auto trimmed = trim_sv(text);
        if (!trimmed.empty() && (trimmed.front() == '{' || trimmed.front() == '['))
        {
            parse_tool_json_segment(trimmed, actions.tool_calls);
        }
    }

    // Assign ids
    for (size_t i = 0; i < actions.tool_calls.size(); ++i)
    {
        if (actions.tool_calls[i].id.empty())
        {
            actions.tool_calls[i].id = std::to_string(i + 1);
        }
    }

    // Drop empty question
    if (actions.question && actions.question->text.empty() && actions.question->answers.empty())
    {
        actions.question.reset();
    }

    actions.visible_text = strip_special_blocks(text);
    return actions;
}

std::string default_agent_system_prompt(std::string_view extra, bool allow_questions)
{
    std::ostringstream ss;
    ss << "You are a coding agent. Solve the user's task carefully.\n\n";
    ss << "You cannot run commands yourself. When you need the client to run something, "
          "emit one or more tool calls. The client executes them and returns results.\n\n";
    ss << "Tool call format (valid JSON inside the tag):\n";
    ss << "<tool_call>\n";
    ss << "{\"name\": \"execute_command\", \"arguments\": {\"command\": \"ls -la\"}}\n";
    ss << "</tool_call>\n\n";
    ss << "Supported tool names (client-side):\n";
    ss << "- execute_command: {\"command\": \"<shell>\"}\n";
    ss << "- read_file: {\"path\": \"<path>\"}\n";
    ss << "- write_file: {\"path\": \"<path>\", \"content\": \"...\"}\n";
    ss << "- list_directory: {\"path\": \"<path>\"}\n";
    ss << "- file_search: {\"query\": \"...\"}\n";
    ss << "- search_text: {\"query\": \"...\", \"path\": \"<optional>\"}\n\n";
    if (allow_questions)
    {
        ss << "When several approaches are reasonable and you need the user to choose, ask with:\n";
        ss << "<question>\nYour question text\n</question>\n";
        ss << "<answer>Option A</answer>\n";
        ss << "<answer>Option B</answer>\n";
        ss << "(Include 2+ <answer> options when choices are clear; omit <answer> for free-form.)\n\n";
        ss << "Do not invent tool results. Wait for the next user/tool message.\n";
        ss << "When finished, reply with a clear final answer and no tool_call/question tags.\n";
    }
    else
    {
        ss << "Do not invent tool results. Wait for the next user/tool message.\n";
        ss << "Do not use <question> or <answer> tags. Prefer a best-effort approach or "
              "state assumptions instead of asking the user to choose.\n";
        ss << "When finished, reply with a clear final answer and no tool_call tags.\n";
    }
    if (!extra.empty())
    {
        ss << "\n";
        ss << extra;
        ss << "\n";
    }
    return ss.str();
}

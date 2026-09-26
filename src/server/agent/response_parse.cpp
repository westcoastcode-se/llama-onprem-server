#include "../agent/response_parse.hpp"

#include <algorithm>
#include <cctype>
#include <format>
#include <functional>
#include <ranges>

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
    const auto lower = [](char c) { return static_cast<unsigned char>(std::tolower(static_cast<unsigned char>(c))); };
    return std::ranges::equal(a, b, std::equal_to{}, lower, lower);
}

bool extract_tag_body(std::string_view text, std::string_view open_name, size_t open_gt,
                      std::string_view &body, size_t &after_close)
{
    // open_gt points at '>' of opening tag
    const auto close = std::format("</{}>", open_name);
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

ParsedAssistantActions parse_assistant_actions(std::string_view text, const ModelAdapter &adapter)
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
            const size_t before = actions.tool_calls.size();
            adapter.parse_tool_calls(body, actions.tool_calls);
            if (actions.tool_calls.size() == before)
            {
                parse_tool_json_segment(body, actions.tool_calls);
            }
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

    // No tagged block matched. Ask the model adapter, then accept a bare JSON tool object.
    if (actions.tool_calls.empty())
    {
        adapter.parse_tool_calls(text, actions.tool_calls);
        auto trimmed = trim_sv(text);
        if (actions.tool_calls.empty() && !trimmed.empty() && (trimmed.front() == '{' || trimmed.front() == '['))
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

ParsedAssistantActions parse_assistant_actions(std::string_view text)
{
    static const QwenAdapter qwen;
    return parse_assistant_actions(text, qwen);
}

string describe_tool(const ChatTool &tool)
{
    string text = std::format("- {}: {}\n", tool.name, tool.description);
    for (const ToolParameter &parameter : tool_parameters(tool))
    {
        text += std::format("    {}: {}", parameter.name, parameter.type.empty() ? "value" : parameter.type);
        if (!parameter.required)
        {
            text += ", optional";
        }
        if (!parameter.description.empty())
        {
            text += std::format(" ({})", parameter.description);
        }
        text.push_back('\n');
    }
    return text;
}

string default_agent_system_prompt(const ModelAdapter &adapter, std::span<const ChatTool> tools, std::string_view extra,
                                   bool allow_questions)
{
    string prompt = "You are a coding agent. Solve the user's task carefully.\n\n"
                    "You cannot run commands yourself. When you need the client to run something, "
                    "emit one or more tool calls. The client executes them and returns results.\n\n";
    if (tools.empty())
    {
        prompt += "The client did not provide any tools. Answer from the conversation alone.\n\n";
    }
    else
    {
        prompt += adapter.example_call(tools);
        prompt += "Available tools:\n";
        for (const ChatTool &tool : tools)
        {
            prompt += describe_tool(tool);
        }
        prompt.push_back('\n');
    }

    if (allow_questions)
    {
        prompt += "When several approaches are reasonable and you need the user to choose, ask with:\n"
                  "<question>\nYour question text\n</question>\n"
                  "<answer>Option A</answer>\n"
                  "<answer>Option B</answer>\n"
                  "(Include 2+ <answer> options when choices are clear; omit <answer> for free-form.)\n\n"
                  "Do not invent tool results. Wait for the next user/tool message.\n"
                  "When finished, reply with a clear final answer and no tool_call/question tags.\n";
    }
    else
    {
        prompt += "Do not invent tool results. Wait for the next user/tool message.\n"
                  "Do not use <question> or <answer> tags. Prefer a best-effort approach or "
                  "state assumptions instead of asking the user to choose.\n"
                  "When finished, reply with a clear final answer and no tool_call tags.\n";
    }

    if (!extra.empty())
    {
        prompt.push_back('\n');
        prompt.append(extra);
        prompt.push_back('\n');
    }

    return prompt;
}

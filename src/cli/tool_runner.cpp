#include "cli/tool_runner.hpp"

#include "common/tools/tool_subagent.hpp"

#include <algorithm>
#include <filesystem>
#include <initializer_list>
#include <ranges>
#include <sstream>
#include <string>
#include <unistd.h>
#include <utility>

namespace
{

enum class ToolKind
{
    Read,
    Write,
    Shell,
    Network,
    Other
};

ToolKind tool_kind(std::string_view name)
{
    if (name == "read_file" || name == "list_directory" || name == "file_search" || name == "search_text")
    {
        return ToolKind::Read;
    }
    if (name == "edit_file")
    {
        return ToolKind::Write;
    }
    if (name == "execute_command")
    {
        return ToolKind::Shell;
    }
    if (name == "web_fetch" || name == "web_search")
    {
        return ToolKind::Network;
    }
    return ToolKind::Other;
}

bool path_in_workspace(const std::filesystem::path &raw, const std::filesystem::path &cwd)
{
    std::error_code ec;
    const auto root = std::filesystem::weakly_canonical(cwd, ec);
    if (ec)
    {
        return false;
    }
    std::filesystem::path target = raw;
    if (target.empty())
    {
        return false;
    }
    if (!target.is_absolute())
    {
        target = cwd / target;
    }
    const auto canon = std::filesystem::weakly_canonical(target, ec);
    if (ec)
    {
        return false;
    }
    const auto rel = std::filesystem::relative(canon, root, ec);
    if (ec)
    {
        return false;
    }
    const auto text = rel.generic_string();
    return text != ".." && !text.starts_with("../");
}

std::string arg_text(const json &args, const char *key)
{
    if (!args.is_object() || !args.contains(key) || args[key].is_null())
    {
        return {};
    }
    if (args[key].is_string())
    {
        return args[key].get<std::string>();
    }
    return args[key].dump();
}

bool tool_is_always(const AgentState &state, std::string_view name)
{
    return is_tool_allowed(name, false, state.always_tools);
}

bool needs_approval(const AgentState &state, std::string_view name, const json &args)
{
    if (state.approval == ApprovalMode::Full || tool_is_always(state, name))
    {
        return false;
    }
    switch (tool_kind(name))
    {
    case ToolKind::Read:
        return false;
    case ToolKind::Write:
        if (state.approval == ApprovalMode::Auto && path_in_workspace(arg_text(args, "path"), state.cwd))
        {
            return false;
        }
        return true;
    case ToolKind::Shell:
    case ToolKind::Network:
    case ToolKind::Other:
        return true;
    }
    return true;
}

// The collapsed tool line: the name the model used, then the text that tool chose.
// The brief is capped so the row stays one line.
std::string tool_summary(std::span<const Tool> tools, std::string_view name, const json &args)
{
    std::string brief;
    if (const Tool *tool = find_tool(tools, name); tool != nullptr && tool->present)
    {
        brief = tool->present(args);
    }
    if (brief.size() > 120)
    {
        brief.resize(117);
        brief += "...";
    }
    std::string summary(name);
    if (!brief.empty())
    {
        summary += ' ';
        summary += brief;
    }
    return summary;
}

std::string first_arg(const json &args, std::initializer_list<const char *> keys)
{
    for (const char *key : keys)
    {
        std::string value = arg_text(args, key);
        if (!value.empty())
        {
            return value;
        }
    }
    return {};
}

// Target stored with the tool result so a later one-line record can name it.
std::string tool_subject(std::string_view name, const json &args)
{
    std::string brief;
    if (name == "execute_command")
    {
        brief = arg_text(args, "command");
    }
    else if (name == "web_fetch")
    {
        brief = arg_text(args, "url");
    }
    else if (name == "web_search")
    {
        brief = arg_text(args, "query");
    }
    else if (name == "sub_agent")
    {
        brief = Tools::subagent_request_text(args);
    }
    else if (name == "search_text")
    {
        brief = first_arg(args, {"query", "pattern", "text", "search"});
    }
    else if (name == "file_search")
    {
        brief = first_arg(args, {"pattern", "query", "name"});
    }
    else
    {
        brief = first_arg(args, {"path", "url", "query", "command"});
    }
    const auto nl = brief.find('\n');
    if (nl != std::string::npos)
    {
        brief.resize(nl);
    }
    if (brief.size() > 160)
    {
        brief.resize(157);
        brief += "...";
    }
    return brief;
}

std::string approval_detail(std::string_view name, const json &args)
{
    if (name == "edit_file")
    {
        std::string detail = arg_text(args, "path");
        detail += "\n";
        std::istringstream text(arg_text(args, "patch"));
        std::string line;
        int shown = 0;
        while (std::getline(text, line))
        {
            if (shown == 30)
            {
                detail += "...\n";
                break;
            }
            detail += line;
            detail += "\n";
            ++shown;
        }
        return detail;
    }
    if (name == "execute_command")
    {
        return arg_text(args, "command");
    }
    if (name == "sub_agent")
    {
        return Tools::subagent_request_text(args);
    }
    return {};
}

// One tool call in the transcript. The fullscreen session keeps it as a single
// line; exec still prints the same notes as before.
class ToolBlock
{
  public:
    ToolBlock(AgentState &state, std::string summary) : state_(state), summary_(std::move(summary))
    {
        if (state_.exec)
        {
            state_.ui->note("• " + summary_);
            return;
        }
        live_ = true;
        state_.ui->begin("tool");
        state_.ui->caption(summary_);
    }

    ToolBlock(const ToolBlock &) = delete;
    ToolBlock &operator=(const ToolBlock &) = delete;

    ~ToolBlock()
    {
        if (!live_)
        {
            return;
        }
        if (shown_)
        {
            state_.ui->collapse("tool");
        }
        state_.ui->end();
    }

    void add(std::string text)
    {
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
        {
            text.pop_back();
        }
        if (text.empty())
        {
            return;
        }
        if (!live_)
        {
            state_.ui->note(std::move(text));
            return;
        }
        if (body_)
        {
            state_.ui->append("\n");
        }
        state_.ui->append(std::move(text));
        body_ = true;
    }

    void show()
    {
        if (!live_)
        {
            return;
        }
        shown_ = true;
        state_.ui->expand("tool");
    }

    void hide()
    {
        if (!live_)
        {
            return;
        }
        shown_ = false;
        state_.ui->collapse("tool");
    }

    void retitle(std::string text)
    {
        summary_ = std::move(text);
        if (live_)
        {
            state_.ui->caption(summary_);
        }
    }

  private:
    AgentState &state_;
    std::string summary_;
    bool live_ = false;
    bool shown_ = false;
    bool body_ = false;
};

Ask ask_approval(std::string_view name, AgentState &state)
{
    if (state.exec && !isatty(STDIN_FILENO))
    {
        state.ui->note("denied " + std::string(name) + " (exec has no terminal to approve it)");
        return Ask::Deny;
    }
    return state.ui->ask("Allow " + std::string(name) + "?", "y yes, n no, a always this tool, f full access");
}

} // namespace

std::optional<json> ToolRunner::run(const SessionResponse &session, std::span<const Tool> tools)
{
    json results = json::array();
    for (const ParsedToolCall &call : session.pending_tool_calls)
    {
        json args = call.arguments.is_object() ? call.arguments : json::object();
        const std::string summary = tool_summary(tools, call.name, args);
        json item{{"id", call.id},
                  {"name", call.name},
                  {"detail", tool_subject(call.name, args)},
                  {"denied", false},
                  {"content", ""}};
        // Ctrl-C already landed. Keep a result for every call so the server can leave AwaitingTools.
        if (g_agent_interrupt.load(std::memory_order_relaxed))
        {
            const std::string skipped = "error: not run because the turn was interrupted";
            item["content"] = skipped;
            ToolBlock block(state_, summary);
            block.add(skipped);
            results.push_back(std::move(item));
            continue;
        }
        bool run = true;
        bool became_full = false;
        {
            ToolBlock block(state_, summary);
            if (needs_approval(state_, call.name, args))
            {
                if (!(state_.exec && !isatty(STDIN_FILENO)))
                {
                    block.add(approval_detail(call.name, args));
                }
                block.show();
                switch (ask_approval(call.name, state_))
                {
                case Ask::Once:
                    break;
                case Ask::AlwaysTool:
                    state_.always_tools.emplace_back(call.name);
                    break;
                case Ask::Full:
                    state_.approval = ApprovalMode::Full;
                    became_full = true;
                    break;
                case Ask::Deny:
                    item["denied"] = true;
                    item["content"] = "denied by user";
                    if (!state_.exec)
                    {
                        block.retitle(summary + "  denied");
                        block.add("denied");
                    }
                    run = false;
                    break;
                case Ask::Closed:
                    return std::nullopt;
                }
                block.hide();
            }
            else if (tool_kind(call.name) == ToolKind::Write)
            {
                block.add(approval_detail(call.name, args));
            }
            if (run)
            {
                const std::string output = run_tool(tools, call.name, args);
                item["content"] = output;
                if (output.starts_with(Tools::kCommandAbortedPrefix))
                {
                    const auto end = output.find('\n');
                    const auto at = Tools::kCommandAbortedPrefix.size();
                    const auto duration = output.substr(at, (end == std::string::npos ? output.size() : end) - at);
                    block.retitle("aborted after " + duration + "  " + summary);
                }
                std::string preview = output;
                if (preview.size() > 160)
                {
                    preview.resize(157);
                    preview += "...";
                }
                std::ranges::replace(preview, '\n', ' ');
                if (state_.exec)
                {
                    block.add("  " + preview);
                }
                else if (!preview.empty())
                {
                    block.add(preview);
                }
            }
        }
        if (became_full)
        {
            state_.ui->note("approval is now full");
        }
        results.push_back(std::move(item));
    }
    return results;
}

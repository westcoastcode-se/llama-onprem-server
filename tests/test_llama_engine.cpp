#include "cli/visible_text.hpp"
#include "common/span_prefix.hpp"
#include "server/agent/model_adapter.hpp"
#include "server/agent/response_parse.hpp"

#include "chat.h"

#include <fstream>
#include <iostream>
#include <span>
#include <sstream>
#include <string>

namespace
{

int g_fails = 0;

void check(bool cond, const char *expr, int line)
{
    if (!cond)
    {
        std::cerr << "FAIL " << line << ": " << expr << "\n";
        ++g_fails;
    }
}

#define CHECK(cond) check(static_cast<bool>(cond), #cond, __LINE__)

std::string visible(std::string_view text)
{
    VisibleText filter;
    std::string out = filter.feed(text);
    out += filter.finish();
    return out;
}

void test_visible_text()
{
    CHECK(visible("Just the answer.") == "Just the answer.");
    CHECK(visible("I'll read it.\n<tool_call>\n<function=read_file>\n<parameter=path>\n/tmp/a\n</parameter>\n</function>\n"
                  "</tool_call>\nDone.") == "I'll read it.\nDone.");
    CHECK(visible("Before\n<tool_calls>\n{\"name\":\"read_file\"}\n</tool_calls>\nAfter") == "Before\nAfter");
    CHECK(visible("<question>\nWhich one?\n</question>\n<answer>\nThe first\n</answer>\nThanks.") == "Thanks.");
    CHECK(visible("<function=read_file>\n<parameter=path>\nx\n</parameter>\n</function>\nOk.") == "Ok.");
    CHECK(visible("<｜tool▁calls▁begin｜><｜tool▁call▁begin｜>function<｜tool▁sep｜>read_file\n```json\n{}\n```\n"
                  "<｜tool▁call▁end｜><｜tool▁calls▁end｜>\nSeen.") == "Seen.");
    CHECK(visible("a < b and c > d") == "a < b and c > d");

    VisibleText chunked;
    std::string out = chunked.feed("I'll read it.\n<tool_ca");
    out += chunked.feed("ll>\n<function=read_file>\n</function>\n</tool_call>\nDone.");
    out += chunked.finish();
    CHECK(out == "I'll read it.\nDone.");

    VisibleText open;
    CHECK(open.feed("Partial <tool_call>\n<function=read_file>") == "Partial ");
    CHECK(open.finish().empty());
}

void test_prefix()
{
    const int32_t a[] = {1, 2, 3, 4};
    const int32_t b[] = {1, 2, 9};
    CHECK(common_prefix_length(std::span<const int32_t>(a, 4), std::span<const int32_t>(b, 3)) == 2);
    CHECK(common_prefix_length(std::span<const int32_t>(a, 4), std::span<const int32_t>(a, 4)) == 4);
    CHECK(common_prefix_length(std::span<const int32_t>(), std::span<const int32_t>(a, 4)) == 0);
}

void test_tool_parse()
{
    const char *json_call = "<tool_call>\n{\"name\":\"read_file\",\"arguments\":{\"path\":\"/tmp/a.txt\"}}\n</tool_call>";
    auto json_actions = parse_assistant_actions(json_call);
    CHECK(json_actions.tool_calls.size() == 1);
    if (!json_actions.tool_calls.empty())
    {
        CHECK(json_actions.tool_calls[0].name == "read_file");
        CHECK(json_actions.tool_calls[0].arguments.value("path", "") == "/tmp/a.txt");
    }

    const char *qwen_call =
        "<tool_call>\n<function=read_file>\n<parameter=path>\n/tmp/a.txt\n</parameter>\n</function>\n</tool_call>";
    auto qwen_actions = parse_assistant_actions(qwen_call);
    CHECK(qwen_actions.tool_calls.size() == 1);
    if (!qwen_actions.tool_calls.empty())
    {
        CHECK(qwen_actions.tool_calls[0].name == "read_file");
        CHECK(qwen_actions.tool_calls[0].arguments.value("path", "") == "/tmp/a.txt");
    }

    const char *qwen_date =
        "<tool_call>\n<function=execute_command>\n<parameter=command>\ndate \"+%Y-%m-%d %H:%M %Z\"\n</parameter>\n</function>\n</tool_call>";
    auto date_actions = parse_assistant_actions(qwen_date);
    CHECK(date_actions.tool_calls.size() == 1);
    if (!date_actions.tool_calls.empty())
    {
        CHECK(date_actions.tool_calls[0].name == "execute_command");
        CHECK(date_actions.tool_calls[0].arguments.value("command", "") == "date \"+%Y-%m-%d %H:%M %Z\"");
    }

    BonsaiAdapter bonsai;
    auto bonsai_actions = parse_assistant_actions(qwen_date, bonsai);
    CHECK(bonsai_actions.tool_calls.size() == 1);
    if (!bonsai_actions.tool_calls.empty())
    {
        CHECK(bonsai_actions.tool_calls[0].name == "execute_command");
    }

    const char *deepseek_call =
        "<｜tool▁calls▁begin｜><｜tool▁call▁begin｜>function<｜tool▁sep｜>execute_command\n"
        "```json\n"
        "{\"command\": \"date\"}\n"
        "```\n"
        "<｜tool▁call▁end｜><｜tool▁calls▁end｜>";
    DeepseekAdapter deepseek;
    auto deepseek_actions = parse_assistant_actions(deepseek_call, deepseek);
    CHECK(deepseek_actions.tool_calls.size() == 1);
    if (!deepseek_actions.tool_calls.empty())
    {
        CHECK(deepseek_actions.tool_calls[0].name == "execute_command");
        CHECK(deepseek_actions.tool_calls[0].arguments.value("command", "") == "date");
    }

    const char *deepseek_v3 =
        "<｜tool▁call▁begin｜>execute_command<｜tool▁sep｜>{\"command\":\"date\"}<｜tool▁call▁end｜>";
    auto v3_actions = parse_assistant_actions(deepseek_v3, deepseek);
    CHECK(v3_actions.tool_calls.size() == 1);
    if (!v3_actions.tool_calls.empty())
    {
        CHECK(v3_actions.tool_calls[0].name == "execute_command");
        CHECK(v3_actions.tool_calls[0].arguments.value("command", "") == "date");
    }

    const char *true_cmd =
        "<tool_call>\n<function=execute_command>\n<parameter=command>\ntrue\n</parameter>\n</function>\n</tool_call>";
    auto true_actions = parse_assistant_actions(true_cmd);
    CHECK(true_actions.tool_calls.size() == 1);
    if (!true_actions.tool_calls.empty())
    {
        CHECK(true_actions.tool_calls[0].arguments.at("command").is_string());
        CHECK(true_actions.tool_calls[0].arguments.at("command").get<std::string>() == "true");
    }

    const char *spaced =
        "<tool_call>\n<function=write_file>\n<parameter=content>\n  hello  \n</parameter>\n</function>\n</tool_call>";
    auto spaced_actions = parse_assistant_actions(spaced);
    CHECK(spaced_actions.tool_calls.size() == 1);
    if (!spaced_actions.tool_calls.empty())
    {
        CHECK(spaced_actions.tool_calls[0].arguments.at("content").get<std::string>() == "  hello  ");
    }

    const char *hybrid = "<tool_call>\n<function=read_file>{\"path\":\"/tmp/a.txt\"}</function>\n</tool_call>";
    auto hybrid_actions = parse_assistant_actions(hybrid);
    CHECK(hybrid_actions.tool_calls.size() == 1);
    if (!hybrid_actions.tool_calls.empty())
    {
        CHECK(hybrid_actions.tool_calls[0].arguments.value("path", "") == "/tmp/a.txt");
    }

    const char *stringified =
        "<tool_call>\n{\"name\":\"read_file\",\"arguments\":\"{\\\"path\\\":\\\"/tmp/a.txt\\\"}\"}\n</tool_call>";
    auto stringified_actions = parse_assistant_actions(stringified);
    CHECK(stringified_actions.tool_calls.size() == 1);
    if (!stringified_actions.tool_calls.empty())
    {
        CHECK(stringified_actions.tool_calls[0].arguments.value("path", "") == "/tmp/a.txt");
    }

    const ThinkingSplit open_think = split_thinking_channel("plan <tool_call>nope</tool_call>", true);
    CHECK(!open_think.closed);
    CHECK(open_think.visible.empty());
    CHECK(parse_assistant_actions(open_think.visible).tool_calls.empty());

    const ThinkingSplit closed_think = split_thinking_channel(
        "plan</think>\n\n<tool_call>\n<function=read_file>\n<parameter=path>\n/tmp/a.txt\n</parameter>\n</function>\n</tool_call>",
        true);
    CHECK(closed_think.closed);
    CHECK(closed_think.reasoning == "plan");
    auto after_think = parse_assistant_actions(closed_think.visible);
    CHECK(after_think.tool_calls.size() == 1);

    const char *inside =
        "<think>draft <tool_call><function=execute_command><parameter=command>rm</parameter></function></tool_call></think>"
        "<tool_call>\n<function=read_file>\n<parameter=path>\n/tmp/a.txt\n</parameter>\n</function>\n</tool_call>";
    const ThinkingSplit explicit_think = split_thinking_channel(inside, false);
    CHECK(explicit_think.closed);
    auto explicit_actions = parse_assistant_actions(explicit_think.visible);
    CHECK(explicit_actions.tool_calls.size() == 1);
    if (!explicit_actions.tool_calls.empty())
    {
        CHECK(explicit_actions.tool_calls[0].name == "read_file");
    }

    ChatTool offset_tool;
    offset_tool.name = "read_file";
    offset_tool.parameters =
        R"({"type":"object","properties":{"offset":{"type":"integer"},"path":{"type":"string"}}})";
    const char *offset_call =
        "<tool_call>\n<function=read_file>\n<parameter=path>\n/tmp/a.txt\n</parameter>\n<parameter=offset>\n10\n</parameter>\n</function>\n</tool_call>";
    auto offset_actions = parse_assistant_actions(offset_call);
    coerce_tool_arguments(offset_actions.tool_calls, std::span<const ChatTool>(&offset_tool, 1));
    CHECK(offset_actions.tool_calls.size() == 1);
    if (!offset_actions.tool_calls.empty())
    {
        CHECK(offset_actions.tool_calls[0].arguments.at("offset") == 10);
        CHECK(offset_actions.tool_calls[0].arguments.at("path").is_string());
    }

    ChatTool union_tool;
    union_tool.name = "ping";
    union_tool.parameters =
        R"({"type":"object","properties":{"host":{"type":["string","null"],"description":"hostname"}}})";
    const auto union_params = tool_parameters(union_tool);
    CHECK(union_params.size() == 1);
    if (!union_params.empty())
    {
        CHECK(union_params[0].type == "string");
        CHECK(union_params[0].description == "hostname");
    }
}

void test_system_prompt_uses_client_tools()
{
    ChatTool tool;
    tool.name = "ping";
    tool.description = "Ping a host";
    tool.parameters =
        R"({"type":"object","properties":{"host":{"type":"string","description":"hostname"}},"required":["host"]})";
    const std::span<const ChatTool> tools(&tool, 1);

    const QwenAdapter qwen;
    const std::string qwen_prompt = default_agent_system_prompt(qwen, tools, "Be brief", true);
    CHECK(qwen_prompt.find("<function=ping>") != std::string::npos);
    CHECK(qwen_prompt.find("<parameter=host>") != std::string::npos);
    CHECK(qwen_prompt.find("Ping a host") != std::string::npos);
    CHECK(qwen_prompt.find("Be brief") != std::string::npos);
    CHECK(qwen_prompt.find("execute_command") == std::string::npos);

    const DeepseekAdapter deepseek;
    const std::string deepseek_prompt = default_agent_system_prompt(deepseek, tools, "", false);
    CHECK(deepseek_prompt.find("<｜tool▁sep｜>ping") != std::string::npos);
    CHECK(deepseek_prompt.find("\"host\"") != std::string::npos);
    CHECK(deepseek_prompt.find("When several approaches") == std::string::npos);
}

void test_model_adapter_selection()
{
    const auto qwen = make_model_adapter("models/Qwen3.8-27B.gguf", "");
    CHECK(qwen->name() == "qwen");

    const auto deepseek = make_model_adapter("models/DeepSeek-V3.gguf", "");
    CHECK(deepseek->name() == "deepseek");

    const auto bonsai = make_model_adapter("models/Qwen3.gguf", "templates/Ternary-Bonsai-2-27B-gguf.jinja");
    CHECK(bonsai->name() == "bonsai");

    const auto fallback = make_model_adapter("models/unknown.gguf", "");
    CHECK(fallback->name() == "qwen");

    const auto qwen_in_deepseek_dir = make_model_adapter("models/Qwen3.gguf", "/opt/deepseek/Qwen3.8-27B.jinja");
    CHECK(qwen_in_deepseek_dir->name() == "qwen");
}

std::string read_file(const std::string &path)
{
    std::ifstream in(path);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

void test_qwen_template()
{
    const std::string src = read_file(LLAMA_ENGINE_TEMPLATE);
    CHECK(!src.empty());
    if (src.empty())
    {
        return;
    }

    common_chat_templates_ptr tmpls = common_chat_templates_init(nullptr, src);
    CHECK(tmpls != nullptr);
    if (!tmpls)
    {
        return;
    }

    auto apply = [&](bool thinking, bool with_tool, bool add_assistant) {
        common_chat_templates_inputs inputs;
        inputs.use_jinja = true;
        inputs.enable_thinking = thinking;
        inputs.add_generation_prompt = add_assistant;
        common_chat_msg user;
        user.role = "user";
        user.content = "hello";
        inputs.messages.push_back(user);
        if (!add_assistant)
        {
            common_chat_msg assistant;
            assistant.role = "assistant";
            assistant.content = "hi there";
            inputs.messages.push_back(std::move(assistant));
        }
        if (with_tool)
        {
            inputs.tools.push_back(common_chat_tool{"read_file", "Read a file", R"({"type":"object","properties":{}})"});
        }
        return common_chat_templates_apply(tmpls.get(), inputs).prompt;
    };

    try
    {
        const std::string thinking = apply(true, false, true);
        CHECK(thinking.find("<|im_start|>assistant\n<think>\n") != std::string::npos);
        CHECK(thinking.find("<think>\n\n</think>") == std::string::npos);

        const std::string quiet = apply(false, false, true);
        CHECK(quiet.find("<think>\n\n</think>\n\n") != std::string::npos);

        const std::string with_tool = apply(true, true, true);
        CHECK(with_tool.find("read_file") != std::string::npos);
        CHECK(with_tool.find("<tools>") != std::string::npos);

        const std::string done = apply(true, false, false);
        CHECK(done.find("hi there") != std::string::npos);
        CHECK(done.find("<|im_end|>") != std::string::npos);
    }
    catch (const std::exception &e)
    {
        std::cerr << "FAIL template apply: " << e.what() << "\n";
        ++g_fails;
    }
}

} // namespace

int main()
{
    test_visible_text();
    test_prefix();
    test_tool_parse();
    test_system_prompt_uses_client_tools();
    test_model_adapter_selection();
    test_qwen_template();
    if (g_fails != 0)
    {
        std::cerr << g_fails << " failed\n";
        return 1;
    }
    std::cout << "ok\n";
    return 0;
}

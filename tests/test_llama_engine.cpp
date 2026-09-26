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

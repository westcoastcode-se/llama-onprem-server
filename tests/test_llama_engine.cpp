#include "cli/visible_text.hpp"
#include "common/span_prefix.hpp"
#include "server/agent/model_adapter.hpp"
#include "server/agent/response_parse.hpp"
#include "server/llm/kv_match.hpp"
#include "tests.hpp"

#include "chat.h"

#include <fstream>
#include <sstream>
#include <string>

extern int test_context_params();
extern int test_session_gc();
extern int test_tool_history();

namespace
{

std::string visible(std::string_view text)
{
    VisibleText filter;
    std::string out = filter.feed(text);
    out += filter.finish();
    return out;
}

std::string read_template(const std::string &path)
{
    std::ifstream in(path);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

/**
 * Plain text is shown as it is.
 */
int test_visible_text_plain()
{
    assertEquals("Just the answer.", visible("Just the answer."));
    assertEquals("a < b and c > d", visible("a < b and c > d"));
    return EXIT_SUCCESS;
}

/**
 * A finished tool call is hidden. The answer on either side stays.
 */
int test_visible_text_tool_call()
{
    assertEquals("I'll read it.\nDone.",
                 visible("I'll read it.\n<tool_call>\n<function=read_file>\n<parameter=path>\n/tmp/a\n</parameter>\n"
                         "</function>\n</tool_call>\nDone."));
    assertEquals("Before\nAfter", visible("Before\n<tool_calls>\n{\"name\":\"read_file\"}\n</tool_calls>\nAfter"));
    assertEquals("Ok.", visible("<function=read_file>\n<parameter=path>\nx\n</parameter>\n</function>\nOk."));
    return EXIT_SUCCESS;
}

/**
 * Question and answer tags are hidden.
 */
int test_visible_text_question()
{
    assertEquals("Thanks.", visible("<question>\nWhich one?\n</question>\n<answer>\nThe first\n</answer>\nThanks."));
    return EXIT_SUCCESS;
}

/**
 * Deepseek tool markers are hidden.
 */
int test_visible_text_deepseek()
{
    assertEquals("Seen.",
                 visible("<｜tool▁calls▁begin｜><｜tool▁call▁begin｜>function<｜tool▁sep｜>read_file\n```json\n{}\n```\n"
                         "<｜tool▁call▁end｜><｜tool▁calls▁end｜>\nSeen."));
    return EXIT_SUCCESS;
}

/**
 * A tool call split across chunks is still hidden.
 */
int test_visible_text_chunked()
{
    VisibleText chunked;
    std::string out = chunked.feed("I'll read it.\n<tool_ca");
    out += chunked.feed("ll>\n<function=read_file>\n</function>\n</tool_call>\nDone.");
    out += chunked.finish();
    assertEquals("I'll read it.\nDone.", out);
    return EXIT_SUCCESS;
}

/**
 * An unfinished tool call is dropped when the stream ends.
 */
int test_visible_text_open()
{
    VisibleText open;
    assertEquals("Partial ", open.feed("Partial <tool_call>\n<function=read_file>"));
    assertEquals("", open.finish());
    return EXIT_SUCCESS;
}

/**
 * The shared token prefix stops at the first difference.
 */
int test_prefix()
{
    const int32_t a[] = {1, 2, 3, 4};
    const int32_t b[] = {1, 2, 9};
    assertEquals(static_cast<size_t>(2), common_prefix_length(std::span<const int32_t>(a, 4), std::span<const int32_t>(b, 3)));
    assertEquals(static_cast<size_t>(4), common_prefix_length(std::span<const int32_t>(a, 4), std::span<const int32_t>(a, 4)));
    assertEquals(static_cast<size_t>(0), common_prefix_length(std::span<const int32_t>(), std::span<const int32_t>(a, 4)));
    return EXIT_SUCCESS;
}

/**
 * A JSON tool call names the tool and keeps the arguments.
 */
int test_tool_parse_json()
{
    const char *json_call = "<tool_call>\n{\"name\":\"read_file\",\"arguments\":{\"path\":\"/tmp/a.txt\"}}\n</tool_call>";
    const auto actions = parse_assistant_actions(json_call);
    assertEquals(1, static_cast<int>(actions.tool_calls.size()));
    assertEquals("read_file", actions.tool_calls[0].name);
    assertEquals("/tmp/a.txt", actions.tool_calls[0].arguments.value("path", ""));
    assertEquals("1", actions.tool_calls[0].id);
    assertTrue(actions.has_client_work());
    return EXIT_SUCCESS;
}

/**
 * Qwen parameter tags become arguments. Surrounding spaces inside a value stay.
 */
int test_tool_parse_qwen()
{
    const char *qwen_call =
        "<tool_call>\n<function=read_file>\n<parameter=path>\n/tmp/a.txt\n</parameter>\n</function>\n</tool_call>";
    const auto actions = parse_assistant_actions(qwen_call);
    assertEquals(1, static_cast<int>(actions.tool_calls.size()));
    assertEquals("read_file", actions.tool_calls[0].name);
    assertEquals("/tmp/a.txt", actions.tool_calls[0].arguments.value("path", ""));

    const char *spaced =
        "<tool_call>\n<function=write_file>\n<parameter=content>\n  hello  \n</parameter>\n</function>\n</tool_call>";
    const auto spaced_actions = parse_assistant_actions(spaced);
    assertEquals(1, static_cast<int>(spaced_actions.tool_calls.size()));
    assertEquals("  hello  ", spaced_actions.tool_calls[0].arguments.at("content").get<std::string>());
    return EXIT_SUCCESS;
}

/**
 * A command value keeps its quotes, and the word true stays a string.
 */
int test_tool_parse_command_string()
{
    const char *qwen_date =
        "<tool_call>\n<function=execute_command>\n<parameter=command>\ndate \"+%Y-%m-%d %H:%M %Z\"\n</parameter>\n</function>\n</tool_call>";
    const auto date_actions = parse_assistant_actions(qwen_date);
    assertEquals(1, static_cast<int>(date_actions.tool_calls.size()));
    assertEquals("execute_command", date_actions.tool_calls[0].name);
    assertEquals("date \"+%Y-%m-%d %H:%M %Z\"", date_actions.tool_calls[0].arguments.value("command", ""));

    BonsaiAdapter bonsai;
    const auto bonsai_actions = parse_assistant_actions(qwen_date, bonsai);
    assertEquals(1, static_cast<int>(bonsai_actions.tool_calls.size()));
    assertEquals("execute_command", bonsai_actions.tool_calls[0].name);

    const char *true_cmd =
        "<tool_call>\n<function=execute_command>\n<parameter=command>\ntrue\n</parameter>\n</function>\n</tool_call>";
    const auto true_actions = parse_assistant_actions(true_cmd);
    assertEquals(1, static_cast<int>(true_actions.tool_calls.size()));
    assertTrue(true_actions.tool_calls[0].arguments.at("command").is_string());
    assertEquals("true", true_actions.tool_calls[0].arguments.at("command").get<std::string>());
    return EXIT_SUCCESS;
}

/**
 * Deepseek fenced JSON and the compact V3 form both parse.
 */
int test_tool_parse_deepseek()
{
    const char *deepseek_call =
        "<｜tool▁calls▁begin｜><｜tool▁call▁begin｜>function<｜tool▁sep｜>execute_command\n"
        "```json\n"
        "{\"command\": \"date\"}\n"
        "```\n"
        "<｜tool▁call▁end｜><｜tool▁calls▁end｜>";
    DeepseekAdapter deepseek;
    const auto actions = parse_assistant_actions(deepseek_call, deepseek);
    assertEquals(1, static_cast<int>(actions.tool_calls.size()));
    assertEquals("execute_command", actions.tool_calls[0].name);
    assertEquals("date", actions.tool_calls[0].arguments.value("command", ""));

    const char *deepseek_v3 =
        "<｜tool▁call▁begin｜>execute_command<｜tool▁sep｜>{\"command\":\"date\"}<｜tool▁call▁end｜>";
    const auto v3_actions = parse_assistant_actions(deepseek_v3, deepseek);
    assertEquals(1, static_cast<int>(v3_actions.tool_calls.size()));
    assertEquals("execute_command", v3_actions.tool_calls[0].name);
    assertEquals("date", v3_actions.tool_calls[0].arguments.value("command", ""));
    return EXIT_SUCCESS;
}

/**
 * JSON inside a function tag, and a stringified arguments field, both parse.
 */
int test_tool_parse_hybrid_and_stringified()
{
    const char *hybrid = "<tool_call>\n<function=read_file>{\"path\":\"/tmp/a.txt\"}</function>\n</tool_call>";
    const auto hybrid_actions = parse_assistant_actions(hybrid);
    assertEquals(1, static_cast<int>(hybrid_actions.tool_calls.size()));
    assertEquals("/tmp/a.txt", hybrid_actions.tool_calls[0].arguments.value("path", ""));

    const char *stringified =
        "<tool_call>\n{\"name\":\"read_file\",\"arguments\":\"{\\\"path\\\":\\\"/tmp/a.txt\\\"}\"}\n</tool_call>";
    const auto stringified_actions = parse_assistant_actions(stringified);
    assertEquals(1, static_cast<int>(stringified_actions.tool_calls.size()));
    assertEquals("/tmp/a.txt", stringified_actions.tool_calls[0].arguments.value("path", ""));
    return EXIT_SUCCESS;
}

/**
 * An open think block hides tool calls. A closed one does not.
 */
int test_tool_parse_thinking_channel()
{
    const ThinkingSplit open_think = split_thinking_channel("plan <tool_call>nope</tool_call>", true);
    assertTrue(!open_think.closed);
    assertTrue(open_think.visible.empty());
    assertTrue(parse_assistant_actions(open_think.visible).tool_calls.empty());

    const ThinkingSplit closed_think = split_thinking_channel(
        "plan</think>\n\n<tool_call>\n<function=read_file>\n<parameter=path>\n/tmp/a.txt\n</parameter>\n</function>\n</tool_call>",
        true);
    assertTrue(closed_think.closed);
    assertEquals("plan", closed_think.reasoning);
    assertEquals(1, static_cast<int>(parse_assistant_actions(closed_think.visible).tool_calls.size()));

    const char *inside =
        "<think>draft <tool_call><function=execute_command><parameter=command>rm</parameter></function></tool_call></think>"
        "<tool_call>\n<function=read_file>\n<parameter=path>\n/tmp/a.txt\n</parameter>\n</function>\n</tool_call>";
    const ThinkingSplit explicit_think = split_thinking_channel(inside, false);
    assertTrue(explicit_think.closed);
    const auto explicit_actions = parse_assistant_actions(explicit_think.visible);
    assertEquals(1, static_cast<int>(explicit_actions.tool_calls.size()));
    assertEquals("read_file", explicit_actions.tool_calls[0].name);
    return EXIT_SUCCESS;
}

/**
 * Integer parameters are coerced, and a union type keeps the first concrete type.
 */
int test_tool_parse_coerce_and_parameters()
{
    ChatTool offset_tool;
    offset_tool.name = "read_file";
    offset_tool.parameters =
        R"({"type":"object","properties":{"offset":{"type":"integer"},"path":{"type":"string"}}})";
    const char *offset_call =
        "<tool_call>\n<function=read_file>\n<parameter=path>\n/tmp/a.txt\n</parameter>\n<parameter=offset>\n10\n</parameter>\n</function>\n</tool_call>";
    auto offset_actions = parse_assistant_actions(offset_call);
    coerce_tool_arguments(offset_actions.tool_calls, std::span<const ChatTool>(&offset_tool, 1));
    assertEquals(1, static_cast<int>(offset_actions.tool_calls.size()));
    assertEquals(10, offset_actions.tool_calls[0].arguments.at("offset").get<int>());
    assertTrue(offset_actions.tool_calls[0].arguments.at("path").is_string());

    ChatTool union_tool;
    union_tool.name = "ping";
    union_tool.parameters =
        R"({"type":"object","properties":{"host":{"type":["string","null"],"description":"hostname"}}})";
    const auto union_params = tool_parameters(union_tool);
    assertEquals(1, static_cast<int>(union_params.size()));
    assertEquals("string", union_params[0].type);
    assertEquals("hostname", union_params[0].description);
    return EXIT_SUCCESS;
}

/**
 * A question and its answers are parsed, and they count as client work.
 */
int test_tool_parse_question()
{
    const char *text =
        "<question>\nWhich one?\n</question>\n<answer>\nThe first\n</answer>\n<answer>The second</answer>";
    const auto actions = parse_assistant_actions(text);
    assertTrue(actions.question.has_value());
    assertEquals("Which one?", actions.question->text);
    assertEquals(2, static_cast<int>(actions.question->answers.size()));
    assertEquals("The first", actions.question->answers[0]);
    assertEquals("The second", actions.question->answers[1]);
    assertTrue(actions.has_client_work());
    assertTrue(actions.tool_calls.empty());
    return EXIT_SUCCESS;
}

/**
 * The system prompt lists the client's tools in that model's call syntax.
 */
int test_system_prompt_uses_client_tools()
{
    ChatTool tool;
    tool.name = "ping";
    tool.description = "Ping a host";
    tool.parameters =
        R"({"type":"object","properties":{"host":{"type":"string","description":"hostname"}},"required":["host"]})";
    const std::span<const ChatTool> tools(&tool, 1);

    const QwenAdapter qwen;
    const std::string qwen_prompt = default_agent_system_prompt(qwen, tools, "Be brief", true);
    assertTrue(qwen_prompt.find("<function=ping>") != std::string::npos);
    assertTrue(qwen_prompt.find("<parameter=host>") != std::string::npos);
    assertTrue(qwen_prompt.find("Ping a host") != std::string::npos);
    assertTrue(qwen_prompt.find("Be brief") != std::string::npos);
    assertTrue(qwen_prompt.find("Older tool results") != std::string::npos);
    assertTrue(qwen_prompt.find("A sub_agent result stays in full.") != std::string::npos);
    assertTrue(qwen_prompt.find("execute_command") == std::string::npos);
    const std::string quiet = default_agent_system_prompt(qwen, tools, "", true, false);
    assertTrue(quiet.find("Older tool results") == std::string::npos);
    assertTrue(quiet.find("A sub_agent result stays in full.") == std::string::npos);

    const DeepseekAdapter deepseek;
    const std::string deepseek_prompt = default_agent_system_prompt(deepseek, tools, "", false);
    assertTrue(deepseek_prompt.find("<｜tool▁sep｜>ping") != std::string::npos);
    assertTrue(deepseek_prompt.find("\"host\"") != std::string::npos);
    assertTrue(deepseek_prompt.find("When several approaches") == std::string::npos);
    return EXIT_SUCCESS;
}

int test_kv_tail_matches_hybrid_suffix()
{
    assertTrue(kv_tail_matches(0, -1));
    assertTrue(!kv_tail_matches(0, 0));
    assertTrue(kv_tail_matches(84639, 84638));
    // Recurrent tail stayed behind the attention cells. Treating this as a kept prefix
    // writes the next tokens onto cells that are already occupied.
    assertTrue(!kv_tail_matches(84639, 80000));
    assertTrue(!kv_tail_matches(1, -1));
    return EXIT_SUCCESS;
}

/**
 * The template name wins over the model path. An unknown name falls back to Qwen.
 */
int test_model_adapter_selection()
{
    const auto qwen = make_model_adapter("models/Qwen3.8-27B.gguf", "");
    assertEquals("qwen", std::string(qwen->name()));

    const auto deepseek = make_model_adapter("models/DeepSeek-V3.gguf", "");
    assertEquals("deepseek", std::string(deepseek->name()));

    const auto bonsai = make_model_adapter("models/Qwen3.gguf", "templates/Ternary-Bonsai-2-27B-gguf.jinja");
    assertEquals("bonsai", std::string(bonsai->name()));

    const auto fallback = make_model_adapter("models/unknown.gguf", "");
    assertEquals("qwen", std::string(fallback->name()));

    const auto qwen_in_deepseek_dir = make_model_adapter("models/Qwen3.gguf", "/opt/deepseek/Qwen3.8-27B.jinja");
    assertEquals("qwen", std::string(qwen_in_deepseek_dir->name()));
    return EXIT_SUCCESS;
}

} // namespace

int test_llama_engine()
{
    RUN_TEST(test_visible_text_plain);
    RUN_TEST(test_visible_text_tool_call);
    RUN_TEST(test_visible_text_question);
    RUN_TEST(test_visible_text_deepseek);
    RUN_TEST(test_visible_text_chunked);
    RUN_TEST(test_visible_text_open);
    RUN_TEST(test_prefix);
    RUN_TEST(test_tool_parse_json);
    RUN_TEST(test_tool_parse_qwen);
    RUN_TEST(test_tool_parse_command_string);
    RUN_TEST(test_tool_parse_deepseek);
    RUN_TEST(test_tool_parse_hybrid_and_stringified);
    RUN_TEST(test_tool_parse_thinking_channel);
    RUN_TEST(test_tool_parse_coerce_and_parameters);
    RUN_TEST(test_tool_parse_question);
    RUN_TEST(test_system_prompt_uses_client_tools);
    RUN_TEST(test_model_adapter_selection);
    RUN_TEST(test_kv_tail_matches_hybrid_suffix);
    return EXIT_SUCCESS;
}

int main()
{
    if (const int rc = test_llama_engine())
    {
        return rc;
    }
    if (const int rc = test_context_params())
    {
        return rc;
    }
    if (const int rc = test_session_gc())
    {
        return rc;
    }
    if (const int rc = test_tool_history())
    {
        return rc;
    }
    return 0;
}

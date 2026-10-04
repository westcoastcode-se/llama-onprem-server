#include "common/span_prefix.hpp"
#include "server/agent/model_adapter.hpp"
#include "server/agent/response_parse.hpp"
#include "server/llm/kv_match.hpp"
#include "server/llm/kv_trim.hpp"
#include "server/llm/token_offset.hpp"
#include "../tests.hpp"

#include "chat.h"

#include <fstream>
#include <sstream>
#include <string>

namespace
{

std::string read_template(const std::string &path)
{
    std::ifstream in(path);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
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
 * Devstral calls are [TOOL_CALLS]name[ARGS]{json}. Several calls can share one turn.
 */
int test_tool_parse_devstral()
{
    const char *call = "Reading it.\n[TOOL_CALLS]read_file[ARGS]{\"path\":\"/tmp/a.txt\"}";
    DevstralAdapter devstral;
    const auto actions = parse_assistant_actions(call, devstral);
    assertEquals(1, static_cast<int>(actions.tool_calls.size()));
    assertEquals("read_file", actions.tool_calls[0].name);
    assertEquals("/tmp/a.txt", actions.tool_calls[0].arguments.value("path", ""));

    const char *two = "[TOOL_CALLS]write_file[ARGS]{\"content\":\"x}y\"}"
                      "[TOOL_CALLS]execute_command[CALL_ID]abc123456[ARGS]{\"command\":\"date\"}";
    const auto both = parse_assistant_actions(two, devstral);
    assertEquals(2, static_cast<int>(both.tool_calls.size()));
    assertEquals("write_file", both.tool_calls[0].name);
    assertEquals("x}y", both.tool_calls[0].arguments.value("content", ""));
    assertEquals("execute_command", both.tool_calls[1].name);
    assertEquals("date", both.tool_calls[1].arguments.value("command", ""));
    assertTrue(!devstral.prompt_opens_think());
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
 * The system prompt shows that model's call syntax. Tool schemas are not copied into the prompt.
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
    assertTrue(qwen_prompt.find("one step at a time") != std::string::npos);
    assertTrue(qwen_prompt.find("Do not write the whole program in one turn.") != std::string::npos);
    assertTrue(qwen_prompt.find("One step per turn.") != std::string::npos);
    assertTrue(qwen_prompt.find("<function=ping>") != std::string::npos);
    assertTrue(qwen_prompt.find("<parameter=host>") != std::string::npos);
    assertTrue(qwen_prompt.find("Ping a host") == std::string::npos);
    assertTrue(qwen_prompt.find("Available tools:") == std::string::npos);
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

    const DevstralAdapter devstral;
    const std::string devstral_prompt = default_agent_system_prompt(devstral, tools, "", true);
    assertTrue(devstral_prompt.find("[TOOL_CALLS]ping[ARGS]") != std::string::npos);
    assertTrue(devstral_prompt.find("\"host\"") != std::string::npos);
    return EXIT_SUCCESS;
}

int test_checkpoint_uses_server_offsets()
{
    const int32_t active[] = {1, 2, 3, 4, 5, 6};
    const int32_t extended[] = {1, 2, 3, 4, 5, 6, 7};
    const size_t ends[] = {3, 6};
    // Both stored spans match, so the checkpoint is the whole cached sequence.
    assertEquals(static_cast<size_t>(6), checkpoint_from_offsets(active, extended, ends));
    // No stored ends: the same answer as a plain token scan.
    assertEquals(common_prefix_length(std::span<const int32_t>(active), std::span<const int32_t>(extended)),
                 checkpoint_from_offsets(active, extended, std::span<const size_t>{}));

    // The span [0, 3) disagrees, so the scan stops at the tokens before that edit.
    const int32_t edited[] = {1, 2, 9, 4, 5, 6};
    assertEquals(static_cast<size_t>(2), checkpoint_from_offsets(active, edited, ends));

    // One stored end anchors the match. A matching tail is kept; a later edit stops the scan there.
    const size_t kept[] = {3};
    const int32_t tail_edit[] = {1, 2, 3, 4, 9, 6};
    assertEquals(static_cast<size_t>(4), checkpoint_from_offsets(active, tail_edit, kept));
    assertEquals(static_cast<size_t>(6), checkpoint_from_offsets(active, extended, kept));
    return EXIT_SUCCESS;
}

int test_kv_trim_follows_the_cache()
{
    const auto hybrid = make_kv_trim(true);
    assertEquals("qwen", std::string(hybrid->name()));
    const auto attention = make_kv_trim(false);
    assertEquals("suffix", std::string(attention->name()));
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

    const auto devstral =
        make_model_adapter("models/Devstral-Small-2-24B-Instruct-2512-UD-Q4_K_XL.gguf", "");
    assertEquals("devstral", std::string(devstral->name()));
    assertTrue(!devstral->prompt_opens_think());

    const auto devstral_template = make_model_adapter("models/Qwen3.gguf", "templates/Devstral-Small.jinja");
    assertEquals("devstral", std::string(devstral_template->name()));

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
    RUN_TEST(test_prefix);
    RUN_TEST(test_tool_parse_json);
    RUN_TEST(test_tool_parse_qwen);
    RUN_TEST(test_tool_parse_command_string);
    RUN_TEST(test_tool_parse_devstral);
    RUN_TEST(test_tool_parse_deepseek);
    RUN_TEST(test_tool_parse_hybrid_and_stringified);
    RUN_TEST(test_tool_parse_thinking_channel);
    RUN_TEST(test_tool_parse_coerce_and_parameters);
    RUN_TEST(test_tool_parse_question);
    RUN_TEST(test_system_prompt_uses_client_tools);
    RUN_TEST(test_model_adapter_selection);
    RUN_TEST(test_checkpoint_uses_server_offsets);
    RUN_TEST(test_kv_trim_follows_the_cache);
    RUN_TEST(test_kv_tail_matches_hybrid_suffix);
    return EXIT_SUCCESS;
}

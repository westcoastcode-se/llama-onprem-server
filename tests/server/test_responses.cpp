#include "server/responses/responses.hpp"
#include "../tests.hpp"

#include <string>

namespace
{

std::string joined(const ResponsesDeltaFilter::Delta &delta)
{
    return delta.reasoning + "|" + delta.text;
}

int test_responses_string_input()
{
    const auto turn = responses_from_json({
        {"model", "local"},
        {"instructions", "Be brief."},
        {"input", "Hello"},
        {"stream", true},
        {"max_output_tokens", 32},
        {"temperature", 0.2},
        {"prompt_cache_key", "thread/1"},
    });
    assertEquals(std::string("local"), turn.model);
    assertEquals(std::string("Be brief."), turn.instructions);
    assertEquals(std::string("thread_1"), turn.session_id);
    assertTrue(turn.stream);
    assertEquals(32, turn.max_tokens);
    assertTrue(turn.temperature > 0.19f && turn.temperature < 0.21f);
    assertEquals(static_cast<size_t>(1), turn.messages.size());
    assertEquals(std::string("user"), turn.messages[0].role);
    assertEquals(std::string("Hello"), turn.messages[0].content);
    return EXIT_SUCCESS;
}

int test_responses_codex_tool_round()
{
    const auto turn = responses_from_json({
        {"input",
         nlohmann::json::array({
             {{"type", "message"}, {"role", "user"}, {"content", nlohmann::json::array({{{"type", "input_text"}, {"text", "List files"}}})}},
             {{"type", "reasoning"},
              {"summary", nlohmann::json::array()},
              {"content", nullptr},
              {"encrypted_content", ""}},
             {{"type", "reasoning"},
              {"summary", nlohmann::json::array()},
              {"content", nlohmann::json::array({{{"type", "reasoning_text"}, {"text", "Look around"}}})}},
             {{"type", "message"},
              {"role", "assistant"},
              {"content", nlohmann::json::array({{{"type", "output_text"}, {"text", "Checking."}}})}},
             {{"type", "function_call"}, {"call_id", "call_1"}, {"name", "shell"}, {"arguments", "{\"command\":\"ls\"}"}},
             {{"type", "function_call_output"}, {"call_id", "call_1"}, {"output", "README.md"}},
         })},
        {"tools",
         nlohmann::json::array({
             {{"type", "function"},
              {"name", "shell"},
              {"description", "Run a command"},
              {"parameters", {{"type", "object"}, {"properties", {{"command", {{"type", "string"}}}}}}}},
             {{"type", "web_search"}, {"name", "web_search"}},
         })},
        {"previous_response_id", "resp_old"},
    });
    assertEquals(std::string("codex"), turn.session_id);
    assertEquals(static_cast<size_t>(1), turn.tools.size());
    assertEquals(std::string("shell"), turn.tools[0].name);
    assertEquals(static_cast<size_t>(3), turn.messages.size());
    assertEquals(std::string("user"), turn.messages[0].role);
    assertEquals(std::string("List files"), turn.messages[0].content);
    assertEquals(std::string("assistant"), turn.messages[1].role);
    assertEquals(std::string("Look around"), turn.messages[1].reasoning_content);
    assertEquals(std::string("Checking."), turn.messages[1].content);
    assertEquals(static_cast<size_t>(1), turn.messages[1].tool_calls.size());
    assertEquals(std::string("shell"), turn.messages[1].tool_calls[0].name);
    assertEquals(std::string("call_1"), turn.messages[1].tool_calls[0].id);
    assertEquals(std::string("ls"), turn.messages[1].tool_calls[0].arguments.value("command", ""));
    assertEquals(std::string("tool"), turn.messages[2].role);
    assertEquals(std::string("call_1"), turn.messages[2].tool_call_id);
    assertEquals(std::string("README.md"), turn.messages[2].content);
    return EXIT_SUCCESS;
}

int test_responses_rejects_images_and_missing_input()
{
    bool missing = false;
    try
    {
        (void)responses_from_json({{"model", "local"}});
    }
    catch (const BadRequest &)
    {
        missing = true;
    }
    assertTrue(missing);

    bool image = false;
    try
    {
        (void)responses_from_json({{"input", nlohmann::json::array({{{"type", "message"},
                                                               {"role", "user"},
                                                               {"content", nlohmann::json::array({{{"type", "input_image"}, {"image_url", "data:image/png;base64,aa"}}})}}})}});
    }
    catch (const BadRequest &)
    {
        image = true;
    }
    assertTrue(image);
    return EXIT_SUCCESS;
}

int test_response_items_match_codex()
{
    const auto ids = ResponseIds{"resp_1", "rs_1", "msg_1"};
    ResponsesResult result;
    result.reasoning = "Think";
    result.text = "Done";
    ParsedToolCall call;
    call.id = "1";
    call.name = "shell";
    call.arguments = nlohmann::json{{"command", "ls"}};
    result.tool_calls.push_back(std::move(call));

    const auto items = response_output_items(ids, result);
    assertEquals(static_cast<size_t>(3), items.size());
    assertEquals(std::string("reasoning"), items[0].value("type", ""));
    assertEquals(std::string("Think"), items[0].at("content")[0].value("text", ""));
    assertEquals(std::string(""), items[0].value("encrypted_content", "missing"));
    assertTrue(items[0].at("summary").is_array());
    assertEquals(std::string("message"), items[1].value("type", ""));
    assertEquals(std::string("Done"), items[1].at("content")[0].value("text", ""));
    assertEquals(std::string("output_text"), items[1].at("content")[0].value("type", ""));
    assertEquals(std::string("function_call"), items[2].value("type", ""));
    assertEquals(std::string("shell"), items[2].value("name", ""));
    assertEquals(std::string("call_1"), items[2].value("call_id", ""));
    assertEquals(std::string("{\"command\":\"ls\"}"), items[2].value("arguments", ""));

    const auto events = response_terminal_events(ids, "local", result);
    assertEquals(std::string("response.completed"), events.back().value("type", ""));
    assertEquals(std::string("resp_1"), events.back().at("response").value("id", ""));
    assertEquals(std::string("event: response.completed\ndata: "), sse_frame(events.back()).substr(0, 32));
    return EXIT_SUCCESS;
}

int test_delta_filter_hides_tools()
{
    ResponsesDeltaFilter filter;
    auto first = filter.feed("Look.\n<think>abc</thi");
    auto second = filter.feed("nk>Answer <tool_ca");
    auto third = filter.feed("ll>\n<function=shell>\n</function>\n</tool_call>\nDone.");
    auto tail = filter.finish();
    assertEquals(std::string("abc|Look.\n"), joined(first));
    assertEquals(std::string("|Answer "), joined(second));
    assertEquals(std::string("|\nDone."), joined(third));
    assertEquals(std::string("|"), joined(tail));

    ResponsesDeltaFilter devstral;
    auto hidden = devstral.feed("A\n[TOOL_CALLS]shell[ARGS]{\"command\":\"ls\"}");
    auto rest = devstral.feed("more");
    auto end = devstral.finish();
    assertEquals(std::string("|A\n"), joined(hidden));
    assertEquals(std::string("|"), joined(rest));
    assertEquals(std::string("|"), joined(end));

    ResponsesDeltaFilter held;
    auto partial = held.feed("a <");
    auto flushed = held.finish();
    assertEquals(std::string("|a "), joined(partial));
    assertEquals(std::string("|<"), joined(flushed));

    ResponsesDeltaFilter plain;
    auto text = plain.feed("a < b");
    auto done = plain.finish();
    assertEquals(std::string("|a < b"), joined(text));
    assertEquals(std::string("|"), joined(done));

    assertEquals(std::string("Reading it.\n"),
                 responses_visible_text("Reading it.\n[TOOL_CALLS]read_file[ARGS]{\"path\":\"/tmp/a.txt\"}"));
    assertEquals(std::string("Done\n"),
                 responses_visible_text("Done\n<tool_call>\n<function=shell>\n</function>\n</tool_call>"));
    return EXIT_SUCCESS;
}

int test_public_model_id()
{
    assertEquals(std::string("Qwen3.8-27B"), public_model_id("models/Qwen3.8-27B.gguf"));
    assertEquals(std::string("local"), public_model_id(""));
    return EXIT_SUCCESS;
}

} // namespace

int test_responses()
{
    RUN_TEST(test_responses_string_input);
    RUN_TEST(test_responses_codex_tool_round);
    RUN_TEST(test_responses_rejects_images_and_missing_input);
    RUN_TEST(test_response_items_match_codex);
    RUN_TEST(test_delta_filter_hides_tools);
    RUN_TEST(test_public_model_id);
    return EXIT_SUCCESS;
}

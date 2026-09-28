#include "common/tool_history.hpp"
#include "server/agent/model_adapter.hpp"
#include "server/sessions/sessions.hpp"
#include "../tests.hpp"

#include <string>

namespace
{

std::string long_read()
{
    std::string body = "lines 1-4 of 90\n1: alpha\n2: beta\n3: gamma\n4: delta\n";
    body.append(400, 'x');
    return body;
}

std::string long_search()
{
    std::string body = "hit one\nhit two\nhit three\nhit four\n";
    body.append(300, 'y');
    return body;
}

void finish(Session &session, std::string text, ParsedAssistantActions actions = {})
{
    auto job = session.begin_generation();
    job->set_result(std::move(text), JobState::Done, std::move(actions));
    session.complete_job(job);
}

void post_user(Session &session, std::string text)
{
    SessionMessageRequest message;
    message.role = "user";
    message.content = std::move(text);
    session.accept_user_message(message);
}

void post_tools(Session &session, ToolResultItem item)
{
    SessionToolResultsRequest body;
    body.results.push_back(std::move(item));
    session.accept_tool_results(body);
}

int test_shrink_keeps_short_and_records()
{
    const std::string short_error =
        "note\n<tool_response name=\"read_file\" detail=\"src/a.cpp\">\nerror: could not open file\n</tool_response>\n";
    assertEquals(short_error, shrink_tool_responses(short_error));

    const std::string legacy =
        "<tool_response name=\"read_file\">\nomitted read_file, 420 lines\n</tool_response>";
    assertEquals(legacy, shrink_tool_responses(legacy));

    const std::string prose = "the assistant already said where the bug is";
    assertEquals(prose, shrink_tool_responses(prose));

    assertEquals(std::string("<tool_response name=\"read_file\" detail=\"a b c\">"),
                 tool_response_open("read_file", "a<b\"c\nrest"));
    assertEquals(std::string("<tool_response>"), tool_response_open("", ""));
    assertEquals(std::string("<tool_response>"), tool_response_open("read file", "   "));
    assertEquals(std::string("<tool_response detail=\"src/a.cpp\">"), tool_response_open("read file", "src/a.cpp"));
    return EXIT_SUCCESS;
}

int test_shrink_names_the_target()
{
    const std::string text = "before\n<tool_response name=\"read_file\" detail=\"src/cli/tui.cpp\">\n" + long_read() +
                             "</tool_response>\nafter\n"
                             "<tool_response name=\"execute_command\" detail=\"cmake --build\">\n" +
                             long_search() + "</tool_response>\n"
                             "<tool_response>\nerror: denied\n</tool_response>";
    const std::string shrunk = shrink_tool_responses(text);
    assertTrue(shrunk.find("before\n") == 0);
    assertTrue(shrunk.find("read_file src/cli/tui.cpp, lines 1-4 of 90") != std::string::npos);
    assertTrue(shrunk.find("1: alpha") == std::string::npos);
    assertTrue(shrunk.find("execute_command cmake --build, 5 lines") != std::string::npos);
    assertTrue(shrunk.find("hit one") == std::string::npos);
    assertTrue(shrunk.find("error: denied") != std::string::npos);
    assertTrue(shrunk.find("after\n") != std::string::npos);
    assertEquals(shrunk, shrink_tool_responses(shrunk));

    const std::string unnamed = "<tool_response>\n" + long_read() + "</tool_response>";
    const std::string unnamed_shrunk = shrink_tool_responses(unnamed);
    assertTrue(unnamed_shrunk.find("tool, lines 1-4 of 90") != std::string::npos);
    return EXIT_SUCCESS;
}

int test_shrink_keeps_subagent_body()
{
    const std::string findings = long_read();
    const std::string text = "before\n<tool_response name=\"sub_agent\" detail=\"inspect tui\">\n" + findings +
                             "</tool_response>\nafter\n"
                             "<tool_response name=\"read_file\" detail=\"src/cli/tui.cpp\">\n" +
                             findings + "</tool_response>";
    const std::string shrunk = shrink_tool_responses(text);
    assertTrue(shrunk.find("before\n") == 0);
    assertTrue(shrunk.find("name=\"sub_agent\"") != std::string::npos);
    assertTrue(shrunk.find("1: alpha") != std::string::npos);
    assertTrue(shrunk.find("read_file src/cli/tui.cpp, lines 1-4 of 90") != std::string::npos);
    const auto sub_at = shrunk.find("name=\"sub_agent\"");
    const auto read_at = shrunk.find("name=\"read_file\"");
    assertTrue(sub_at < read_at);
    assertTrue(shrunk.find("1: alpha") < read_at);
    assertEquals(shrunk, shrink_tool_responses(shrunk));

    const std::string alias = "<tool_response name=\"spawn_subagent\" detail=\"look\">\n" + findings + "</tool_response>";
    assertEquals(alias, shrink_tool_responses(alias));
    return EXIT_SUCCESS;
}

int test_older_tool_results_shrink_when_the_next_arrives()
{
    QwenAdapter adapter;
    Session session(adapter);
    session.configure({});
    assertTrue(session.to_response().compress_tools);
    assertTrue(session.to_response().system_prompt.find("Older tool results") != std::string::npos);
    assertTrue(session.to_response().system_prompt.find("The latest result stays in full.") != std::string::npos);
    assertTrue(session.to_response().system_prompt.find("A later read of the same file") != std::string::npos);

    post_user(session, "fix it");
    ParsedAssistantActions read;
    ParsedToolCall read_call;
    read_call.name = "read_file";
    read.tool_calls.push_back(std::move(read_call));
    finish(session, "reading tui.cpp", std::move(read));

    ToolResultItem read_item;
    read_item.name = "read_file";
    read_item.detail = "src/cli/tui.cpp";
    read_item.content = long_read();
    post_tools(session, std::move(read_item));
    assertTrue(session.to_response().messages.back().content.find("1: alpha") != std::string::npos);
    assertTrue(session.to_response().messages.back().content.find("name=\"read_file\"") != std::string::npos);

    ParsedAssistantActions search;
    ParsedToolCall search_call;
    search_call.name = "search_text";
    search.tool_calls.push_back(std::move(search_call));
    finish(session, "searching accept", std::move(search));

    ToolResultItem search_item;
    search_item.name = "search_text";
    search_item.detail = "Session::accept";
    search_item.content = long_search();
    post_tools(session, std::move(search_item));
    assertTrue(session.to_response().messages[2].content.find("read_file src/cli/tui.cpp, lines 1-4 of 90") !=
               std::string::npos);
    assertTrue(session.to_response().messages[2].content.find("1: alpha") == std::string::npos);
    assertTrue(session.to_response().messages.back().content.find("hit one") != std::string::npos);

    finish(session, "edited it");
    post_user(session, "next task");

    const SessionResponse stored = session.to_response();
    assertTrue(stored.messages[2].content.find("read_file src/cli/tui.cpp, lines 1-4 of 90") != std::string::npos);
    assertTrue(stored.messages[2].content.find("1: alpha") == std::string::npos);
    assertTrue(stored.messages[4].content.find("search_text Session::accept, 5 lines") != std::string::npos);
    assertTrue(stored.messages[4].content.find("hit one") == std::string::npos);
    assertEquals(std::string("edited it"), stored.messages[5].content);
    assertEquals(std::string("next task"), stored.messages.back().content);

    finish(session, "ok");
    post_user(session, "third");
    const SessionResponse again = session.to_response();
    assertEquals(stored.messages[2].content, again.messages[2].content);
    assertEquals(stored.messages[4].content, again.messages[4].content);
    return EXIT_SUCCESS;
}

int test_subagent_result_survives_the_next_user_turn()
{
    QwenAdapter adapter;
    Session session(adapter);
    session.configure({});
    assertTrue(session.to_response().system_prompt.find("A sub_agent result stays in full.") != std::string::npos);

    post_user(session, "map the tui");
    ParsedAssistantActions delegated;
    ParsedToolCall sub;
    sub.name = "sub_agent";
    delegated.tool_calls.push_back(std::move(sub));
    ParsedToolCall read_call;
    read_call.name = "read_file";
    delegated.tool_calls.push_back(std::move(read_call));
    finish(session, "delegating", std::move(delegated));

    ToolResultItem sub_item;
    sub_item.name = "sub_agent";
    sub_item.detail = "inspect tui";
    sub_item.content = long_read();
    ToolResultItem read_item;
    read_item.name = "read_file";
    read_item.detail = "src/cli/tui.cpp";
    read_item.content = long_read();
    SessionToolResultsRequest body;
    body.results.push_back(std::move(sub_item));
    body.results.push_back(std::move(read_item));
    session.accept_tool_results(body);
    assertTrue(session.to_response().messages.back().content.find("1: alpha") != std::string::npos);

    finish(session, "the caption is set in tui.cpp");
    post_user(session, "next task");

    const std::string stored = session.to_response().messages[2].content;
    assertTrue(stored.find("name=\"sub_agent\"") != std::string::npos);
    assertTrue(stored.find("1: alpha") != std::string::npos);
    assertTrue(stored.find("sub_agent inspect tui,") == std::string::npos);
    assertTrue(stored.find("read_file src/cli/tui.cpp, lines 1-4 of 90") != std::string::npos);
    const auto sub_at = stored.find("name=\"sub_agent\"");
    const auto read_at = stored.find("name=\"read_file\"");
    assertTrue(sub_at < read_at);
    assertTrue(stored.find("1: alpha") < read_at);
    return EXIT_SUCCESS;
}

int test_question_answer_keeps_the_tool_result()
{
    QwenAdapter adapter;
    Session session(adapter);
    session.configure({});
    post_user(session, "fix it");

    ParsedAssistantActions read;
    ParsedToolCall call;
    call.name = "read_file";
    read.tool_calls.push_back(std::move(call));
    finish(session, "reading", std::move(read));

    ToolResultItem item;
    item.name = "read_file";
    item.detail = "src/cli/tui.cpp";
    item.content = long_read();
    post_tools(session, std::move(item));

    ParsedAssistantActions question;
    question.question = ParsedQuestion{.text = "Which one?", .answers = {"a", "b"}};
    finish(session, "Which approach?", std::move(question));
    assertEquals(std::string("awaiting_question"), std::string(session.to_response().state.to_string()));

    post_user(session, "a");
    assertTrue(session.to_response().messages[2].content.find("1: alpha") != std::string::npos);

    finish(session, "edited it");
    post_user(session, "next task");
    assertTrue(session.to_response().messages[2].content.find("read_file src/cli/tui.cpp, lines 1-4 of 90") != std::string::npos);
    assertTrue(session.to_response().messages[2].content.find("1: alpha") == std::string::npos);
    return EXIT_SUCCESS;
}

int test_compress_tools_can_be_off()
{
    QwenAdapter adapter;
    CreateSessionRequest req;
    req.compress_tools = false;
    Session session(adapter);
    session.configure(req);
    assertTrue(!session.to_response().compress_tools);
    assertTrue(session.to_response().system_prompt.find("Older tool results") == std::string::npos);

    post_user(session, "fix it");
    ParsedAssistantActions read;
    ParsedToolCall call;
    call.name = "read_file";
    read.tool_calls.push_back(std::move(call));
    finish(session, "reading", std::move(read));

    ToolResultItem item;
    item.name = "read_file";
    item.detail = "src/cli/tui.cpp";
    item.content = long_read();
    post_tools(session, std::move(item));
    const std::string stored = session.to_response().messages.back().content;
    assertTrue(stored.find("<tool_response>\n") == 0);
    assertTrue(stored.find("name=") == std::string::npos);

    finish(session, "edited it");
    post_user(session, "next task");
    assertTrue(session.to_response().messages[2].content.find("1: alpha") != std::string::npos);

    auto clone = session.capture({});
    Session child(adapter);
    child.load_clone(std::move(clone));
    assertTrue(!child.to_response().compress_tools);
    assertEquals(session.to_response().messages[2].content, child.to_response().messages[2].content);
    return EXIT_SUCCESS;
}

} // namespace

int test_tool_history()
{
    RUN_TEST(test_shrink_keeps_short_and_records);
    RUN_TEST(test_shrink_names_the_target);
    RUN_TEST(test_shrink_keeps_subagent_body);
    RUN_TEST(test_older_tool_results_shrink_when_the_next_arrives);
    RUN_TEST(test_subagent_result_survives_the_next_user_turn);
    RUN_TEST(test_question_answer_keeps_the_tool_result);
    RUN_TEST(test_compress_tools_can_be_off);
    return EXIT_SUCCESS;
}

//
// File containing tests for API model JSON
//

#include "api/models.hpp"
#include "api/sessions.hpp"
#include "../tests.hpp"

/**
 * Job states round-trip through their names. A finished state is done, error, or cancelled.
 */
static int test_job_state() {
    assertEquals("queued", std::string(JobState(JobState::Queued).to_string()));
    assertEquals("running", std::string(JobState(JobState::Running).to_string()));
    assertEquals("done", std::string(JobState(JobState::Done).to_string()));
    assertTrue(JobState(JobState::Done).is_finished());
    assertTrue(JobState(JobState::Error).is_finished());
    assertTrue(JobState(JobState::Cancelled).is_finished());
    assertTrue(!JobState(JobState::Running).is_finished());
    assertTrue(JobState::to_enum("cancelled") == JobState::Cancelled);

    bool threw = false;
    try
    {
        JobState::to_enum("nope");
    }
    catch (const std::runtime_error &error)
    {
        threw = true;
        assertTrue(std::string(error.what()).find("unknown JobState") != std::string::npos);
    }
    assertTrue(threw);
    return EXIT_SUCCESS;
}

/**
 * Session states round-trip through their names.
 */
static int test_session_state() {
    assertEquals("generating", std::string(SessionState(SessionState::Generating).to_string()));
    assertTrue(SessionState(SessionState::Generating).is_running());
    assertTrue(SessionState(SessionState::Idle).is_sleeping());
    assertTrue(SessionState::to_enum("awaiting_tools") == SessionState::AwaitingTools);
    assertTrue(SessionState::to_enum("awaiting_question") == SessionState::AwaitingQuestion);
    return EXIT_SUCCESS;
}

/**
 * A message request needs content and a role.
 */
static int test_session_message_request() {
    bool threw = false;
    try
    {
        SessionMessageRequest req;
        req.role = "user";
        req.validate();
    }
    catch (const BadRequest &)
    {
        threw = true;
    }
    assertTrue(threw);

    const auto req = SessionMessageRequest::from_json({{"content", "hello"}, {"max_tokens", 12}});
    assertEquals("hello", req.content);
    assertEquals("user", req.role);
    assertEquals(12, req.max_tokens);
    assertEquals("hello", req.to_json().value("content", ""));
    return EXIT_SUCCESS;
}

/**
 * Chat tools and messages survive a JSON round trip.
 */
static int test_chat_tool_and_message_json() {
    const json tool_json = {{"type", "function"},
                            {"function",
                             {{"name", "read_file"},
                              {"description", "Read"},
                              {"parameters", {{"type", "object"}}}}}};
    const auto tool = ChatTool::from_json(tool_json);
    assertEquals("read_file", tool.name);
    assertEquals("Read", tool.description);
    assertEquals("read_file", tool.to_json()["function"].value("name", ""));

    bool threw = false;
    try
    {
        ChatTool::from_json(json::object());
    }
    catch (const BadRequest &)
    {
        threw = true;
    }
    assertTrue(threw);

    ChatMessage message;
    message.role = "user";
    message.content = "hello";
    message.reasoning_content = "because";
    const auto back = ChatMessage::from_json(message.to_json());
    assertEquals("user", back.role);
    assertEquals("hello", back.content);
    assertEquals("because", back.reasoning_content);
    return EXIT_SUCCESS;
}

/**
 * A create-session body keeps the system text, tools, and messages.
 */
static int test_create_session_request_json() {
    CreateSessionRequest req;
    req.system = "Be brief";
    req.questions = false;
    req.max_tokens = 32;
    ChatMessage message;
    message.role = "user";
    message.content = "hello";
    req.messages.push_back(message);

    const auto back = CreateSessionRequest::from_json(req.to_json());
    assertEquals("Be brief", back.system);
    assertTrue(!back.questions);
    assertTrue(back.compress_tools);
    assertEquals(32, back.max_tokens);
    assertEquals(1, static_cast<int>(back.messages.size()));
    assertEquals("hello", back.messages[0].content);

    CreateSessionRequest off;
    off.compress_tools = false;
    const auto off_back = CreateSessionRequest::from_json(off.to_json());
    assertTrue(!off_back.compress_tools);
    const auto omitted = CreateSessionRequest::from_json(json::object());
    assertTrue(omitted.compress_tools);
    return EXIT_SUCCESS;
}

/**
 * Error responses keep the code and the message.
 */
static int test_error_response_json() {
    const ErrorResponse error{.error_code = 400, .message = "nope"};
    const auto back = ErrorResponse::from_json(error.to_json());
    assertEquals(400, back.error_code);
    assertEquals("nope", back.message);
    assertEquals("context_full", std::string(kContextFull));
    return EXIT_SUCCESS;
}

/**
 * Run all API model tests
 */
int test_models() {
    RUN_TEST(test_job_state);
    RUN_TEST(test_session_state);
    RUN_TEST(test_session_message_request);
    RUN_TEST(test_chat_tool_and_message_json);
    RUN_TEST(test_create_session_request_json);
    RUN_TEST(test_error_response_json);
    return EXIT_SUCCESS;
}

#include "api/errors.hpp"
#include "server/agent/model_adapter.hpp"
#include "server/jobs/jobs.hpp"
#include "server/llm/llm_engine.hpp"
#include "server/sessions/sessions.hpp"
#include "../tests.hpp"

/**
 * A session the client still has to answer must survive garbage collection.
 * Creating a sub-agent session collects idle ones, and the parent's tool-result
 * POST is what arrives after that sub-agent finishes.
 */
static int test_waiting_session_is_not_gc_idle()
{
    QwenAdapter adapter;

    Session idle(adapter);
    assertTrue(idle.is_gc_idle());

    Session generating(adapter);
    const auto running = generating.begin_generation();
    assertTrue(!generating.is_gc_idle());
    assertTrue(static_cast<bool>(running));

    Session tools(adapter);
    auto tool_task = tools.begin_generation();
    ParsedAssistantActions tool_actions;
    ParsedToolCall call;
    call.name = "sub_agent";
    tool_actions.tool_calls.push_back(std::move(call));
    tool_task->set_result("run it", JobState::Done, std::move(tool_actions));
    tools.complete_job(tool_task);
    assertTrue(!tools.is_gc_idle());

    Session question(adapter);
    auto question_task = question.begin_generation();
    ParsedAssistantActions question_actions;
    ParsedQuestion asked;
    asked.text = "which?";
    question_actions.question = std::move(asked);
    question_task->set_result("ask", JobState::Done, std::move(question_actions));
    question.complete_job(question_task);
    assertTrue(!question.is_gc_idle());

    Session finished(adapter);
    auto done_task = finished.begin_generation();
    done_task->set_result("ok", JobState::Done, {});
    finished.complete_job(done_task);
    assertTrue(finished.is_gc_idle());
    return EXIT_SUCCESS;
}

/**
 * POST /v1/sessions with an id returns that session and does not replace its messages.
 * An id the server does not hold is rejected.
 */
static int test_create_resumes_existing_session()
{
    QwenAdapter adapter;
    LlamaEngine engine;
    Jobs jobs(engine, adapter);
    Sessions sessions(jobs, adapter);

    CreateSessionRequest created_request;
    ChatMessage message;
    message.role = "user";
    message.content = "keep me";
    created_request.messages.push_back(message);
    const auto created = sessions.create(created_request);
    const SessionID id = created->id;
    const std::string prompt = created->to_response().system_prompt;

    CreateSessionRequest again;
    again.id = id;
    again.system = "wipe";
    ChatMessage replacement;
    replacement.role = "user";
    replacement.content = "replace";
    again.messages.push_back(replacement);
    const auto resumed = sessions.create(again);

    assertTrue(created.get() == resumed.get());
    assertEquals(id, resumed->id);
    const SessionResponse body = resumed->to_response();
    assertEquals(prompt, body.system_prompt);
    assertEquals(static_cast<size_t>(1), body.messages.size());
    assertEquals(std::string("keep me"), body.messages[0].content);

    const auto fresh = sessions.create(CreateSessionRequest{});
    assertTrue(fresh->id != id);

    CreateSessionRequest missing;
    missing.id = id == 42 ? 43 : 42;
    bool threw = false;
    std::shared_ptr<Session> resumed_missing;
    try
    {
        resumed_missing = sessions.create(missing);
    }
    catch (const NotFound &)
    {
        threw = true;
    }
    assertTrue(threw);
    assertTrue(resumed_missing == nullptr);
    return EXIT_SUCCESS;
}

/**
 * A cancel that never measured the window must leave the previous count in place.
 * The compact offer reads that count. A later job that did measure replaces it.
 */
static int test_unmeasured_job_keeps_context()
{
    QwenAdapter adapter;
    Session session(adapter);
    session.set_context_usage(5000, 32000);
    auto task = session.begin_generation();
    task->set_cancelled();
    session.complete_job(task);
    const auto [used, size] = session.context_usage();
    assertEquals(5000, used);
    assertEquals(32000, size);

    auto measured = session.begin_generation();
    measured->note_context(9000, 32000);
    measured->set_result("ok", JobState::Done, {});
    session.complete_job(measured);
    const auto [used2, size2] = session.context_usage();
    assertEquals(9000, used2);
    assertEquals(32000, size2);
    return EXIT_SUCCESS;
}

int test_session_gc()
{
    RUN_TEST(test_waiting_session_is_not_gc_idle);
    RUN_TEST(test_create_resumes_existing_session);
    RUN_TEST(test_unmeasured_job_keeps_context);
    return EXIT_SUCCESS;
}

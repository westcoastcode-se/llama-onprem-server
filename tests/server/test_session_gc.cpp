#include "server/agent/model_adapter.hpp"
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

int test_session_gc()
{
    RUN_TEST(test_waiting_session_is_not_gc_idle);
    return EXIT_SUCCESS;
}

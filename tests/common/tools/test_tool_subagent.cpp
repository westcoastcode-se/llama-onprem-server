//
// File containing tests for the sub_agent tool
//

#include "common/tools.hpp"
#include "../../tests.hpp"

/**
 * Without a runner the tool cannot start.
 */
static int test_tool_subagent_error_no_runner() {
    const auto tool = Tools::create_subagent_tool(nullptr);
    const auto result = tool.execute({{"task", "look around"}});
    assertEquals("error: sub-agent runner not configured", result);
    return EXIT_SUCCESS;
}

/**
 * A missing task is an error.
 */
static int test_tool_subagent_error_missing_task() {
    const auto tool = Tools::create_subagent_tool([](std::string_view, bool) { return std::string("ok"); });
    const auto result = tool.execute(nlohmann::json::object());
    assertEquals("error: missing required argument 'task'", result);
    return EXIT_SUCCESS;
}

/**
 * One task returns the runner result unchanged.
 */
static int test_tool_subagent_one_task() {
    const auto tool = Tools::create_subagent_tool([](std::string_view task, bool inherit) {
        return std::string(inherit ? "kept " : "fresh ") + std::string(task);
    });
    const auto result = tool.execute({{"task", "read the file"}});
    assertEquals("fresh read the file", result);
    return EXIT_SUCCESS;
}

/**
 * Several tasks run in order and are labeled.
 */
static int test_tool_subagent_several_tasks() {
    const auto tool = Tools::create_subagent_tool([](std::string_view task, bool) {
        return std::string("did ") + std::string(task);
    });
    const auto result = tool.execute({{"tasks", {"one", "two"}}});
    assertEquals("### Task 1/2: one\ndid one\n\n### Task 2/2: two\ndid two", result);
    return EXIT_SUCCESS;
}

/**
 * Run all sub_agent tests
 */
/**
 * inherit is off unless the caller sets it.
 */
static int test_tool_subagent_inherit_flag() {
    bool inherited = true;
    const auto tool = Tools::create_subagent_tool([&](std::string_view, bool inherit) {
        inherited = inherit;
        return std::string("ok");
    });
    tool.execute({{"task", "look"}, {"inherit", true}});
    assertTrue(inherited);
    tool.execute({{"task", "look"}});
    assertTrue(!inherited);
    return EXIT_SUCCESS;
}

/**
 * A context-full result stops a batch so the parent can ask a smaller question.
 */
static int test_tool_subagent_stops_when_context_full() {
    const auto tool = Tools::create_subagent_tool([](std::string_view task, bool) {
        if (task == "one")
        {
            return std::string("error: sub-agent ran out of context. shrink");
        }
        return std::string("did ") + std::string(task);
    });
    const auto result = tool.execute({{"tasks", {"one", "two"}}});
    assertEquals("error: sub-agent ran out of context. shrink", result);
    return EXIT_SUCCESS;
}

/**
 * The approval text keeps the whole task.
 */
static int test_subagent_request_text_is_complete() {
    const std::string task(400, 'a');
    const auto text = Tools::subagent_request_text({{"task", task}, {"inherit", true}});
    assertEquals(task + "\ninherit: yes", text);
    const auto tasks = Tools::subagent_request_text({{"tasks", {"one", "two"}}});
    assertEquals("one\ntwo", tasks);
    return EXIT_SUCCESS;
}

int test_tool_subagent() {
    RUN_TEST(test_tool_subagent_error_no_runner);
    RUN_TEST(test_tool_subagent_error_missing_task);
    RUN_TEST(test_tool_subagent_one_task);
    RUN_TEST(test_tool_subagent_several_tasks);
    RUN_TEST(test_tool_subagent_inherit_flag);
    RUN_TEST(test_tool_subagent_stops_when_context_full);
    RUN_TEST(test_subagent_request_text_is_complete);
    return EXIT_SUCCESS;
}

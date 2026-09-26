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
    const auto tool = Tools::create_subagent_tool([](std::string_view) { return std::string("ok"); });
    const auto result = tool.execute(nlohmann::json::object());
    assertEquals("error: missing required argument 'task'", result);
    return EXIT_SUCCESS;
}

/**
 * One task returns the runner result unchanged.
 */
static int test_tool_subagent_one_task() {
    const auto tool = Tools::create_subagent_tool([](std::string_view task) {
        return std::string("did ") + std::string(task);
    });
    const auto result = tool.execute({{"task", "read the file"}});
    assertEquals("did read the file", result);
    return EXIT_SUCCESS;
}

/**
 * Several tasks run in order and are labeled.
 */
static int test_tool_subagent_several_tasks() {
    const auto tool = Tools::create_subagent_tool([](std::string_view task) {
        return std::string("did ") + std::string(task);
    });
    const auto result = tool.execute({{"tasks", {"one", "two"}}});
    assertEquals("### Task 1/2: one\ndid one\n\n### Task 2/2: two\ndid two", result);
    return EXIT_SUCCESS;
}

/**
 * Run all sub_agent tests
 */
int test_tool_subagent() {
    RUN_TEST(test_tool_subagent_error_no_runner);
    RUN_TEST(test_tool_subagent_error_missing_task);
    RUN_TEST(test_tool_subagent_one_task);
    RUN_TEST(test_tool_subagent_several_tasks);
    return EXIT_SUCCESS;
}

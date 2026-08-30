//
// File containing tests for the read_file tool
//


#include "common/tools.hpp"
#include "../../tests.hpp"

/**
 * Verifying a simple execution of an echo command
 */
static int test_tool_execute_command_echo() {
    const auto tool = Tools::create_execute_command_tool();
    const nlohmann::json args = {{"command", "echo 'hello from test'"}};
    const auto result = tool.execute(args);
    assertEquals("Exit code: 0\nOutput:\nhello from test\n", result);
    return EXIT_SUCCESS;
}

/**
 * Verifying a simple execution of an echo command
 */
static int test_tool_execute_command_echo_exitcode_1() {
    const auto tool = Tools::create_execute_command_tool();
    const nlohmann::json args = {{"command", "exit 1"}};
    const auto result = tool.execute(args);
    assertEquals("Exit code: 1\nOutput:\n(no output)", result);
    return EXIT_SUCCESS;
}

/**
 * Verifying that the execute command returns an error when the command argument is missing
 */
static int test_tool_execute_command_error_missing_command() {
    const auto tool = Tools::create_execute_command_tool();
    const nlohmann::json args = {{}};
    const auto result = tool.execute(args);
    assertEquals("error: missing required string argument 'command'", result);
    return EXIT_SUCCESS;
}

/**
 * Verifying that the execute command returns an error when the command argument is an invalid type
 */
static int test_tool_execute_command_error_invalid_command() {
    const auto tool = Tools::create_execute_command_tool();
    const nlohmann::json args = {{"command", 100}};
    const auto result = tool.execute(args);
    assertEquals("error: missing required string argument 'command'", result);
    return EXIT_SUCCESS;
}

/**
 * Run all execute command tests
 */
void test_tool_execute_command() {
    RUN_TEST(test_tool_execute_command_echo);
    RUN_TEST(test_tool_execute_command_echo_exitcode_1);
    RUN_TEST(test_tool_execute_command_error_missing_command);
    RUN_TEST(test_tool_execute_command_error_invalid_command);
}

//
// File containing tests for the read_file tool
//

#include "common/defer.hpp"
#include "common/tools.hpp"
#include "../../tests.hpp"

#include <atomic>
#include <chrono>
#include <optional>
#include <regex>
#include <signal.h>
#include <string>
#include <unistd.h>

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

static std::atomic<std::atomic<bool> *> g_alarm_cancel{nullptr};

static void on_test_alarm(int) {
    if (std::atomic<bool> *flag = g_alarm_cancel.load(std::memory_order_acquire)) {
        flag->store(true, std::memory_order_relaxed);
    }
}

// Milliseconds named by an abort result. Empty when the text is not an abort.
static std::optional<int> aborted_milliseconds(const std::string &result) {
    std::smatch match;
    const std::regex millis("^error: command aborted after (\\d+)ms\n");
    if (std::regex_search(result, match, millis)) {
        return std::stoi(match[1].str());
    }
    const std::regex seconds("^error: command aborted after (\\d+)\\.(\\d)s\n");
    if (std::regex_search(result, match, seconds)) {
        return std::stoi(match[1].str()) * 1000 + std::stoi(match[2].str()) * 100;
    }
    return std::nullopt;
}

static int fail_command(const std::string &result, long wall_ms) {
    std::cout << "Assertion failed at " << AI_SHORT_FILENAME << ":" << __LINE__ << ": wall=" << wall_ms
              << "ms result=\"" << result << "\"" << std::endl;
    return EXIT_FAILURE;
}

/**
 * A hooked but clear cancel flag still returns the exit code.
 */
static int test_tool_execute_command_cancel_flag_idle() {
    std::atomic<bool> cancel{false};
    Tools::set_execute_command_cancel(&cancel);
    defer(Tools::set_execute_command_cancel(nullptr));
    const auto tool = Tools::create_execute_command_tool();
    const auto result = tool.execute({{"command", "echo 'hello from test'"}});
    assertEquals("Exit code: 0\nOutput:\nhello from test\n", result);
    return EXIT_SUCCESS;
}

/**
 * Ctrl-C before the process starts does not wait for the command.
 */
static int test_tool_execute_command_abort_before_start() {
    std::atomic<bool> cancel{true};
    Tools::set_execute_command_cancel(&cancel);
    defer(Tools::set_execute_command_cancel(nullptr));
    const auto tool = Tools::create_execute_command_tool();
    const auto started = std::chrono::steady_clock::now();
    const auto result = tool.execute({{"command", "sleep 5"}});
    const auto wall = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started)
                          .count();
    const auto ran = aborted_milliseconds(result);
    if (!ran || *ran > 500 || wall > 1000 || result.find("(no output)") == std::string::npos) {
        return fail_command(result, wall);
    }
    return EXIT_SUCCESS;
}

/**
 * Ctrl-C while the command is running kills it and reports how long it ran.
 */
static int test_tool_execute_command_abort_while_running() {
    std::atomic<bool> cancel{false};
    Tools::set_execute_command_cancel(&cancel);
    g_alarm_cancel.store(&cancel, std::memory_order_release);
    struct sigaction action {};
    action.sa_handler = on_test_alarm;
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0;
    struct sigaction previous {};
    if (sigaction(SIGALRM, &action, &previous) != 0) {
        Tools::set_execute_command_cancel(nullptr);
        g_alarm_cancel.store(nullptr, std::memory_order_release);
        std::cout << "Assertion failed at " << AI_SHORT_FILENAME << ":" << __LINE__ << ": sigaction failed"
                  << std::endl;
        return EXIT_FAILURE;
    }
    defer(Tools::set_execute_command_cancel(nullptr));
    defer(sigaction(SIGALRM, &previous, nullptr));
    defer(g_alarm_cancel.store(nullptr, std::memory_order_release));
    defer(alarm(0));
    alarm(1);

    const auto tool = Tools::create_execute_command_tool();
    const auto started = std::chrono::steady_clock::now();
    const auto result = tool.execute({{"command", "/bin/echo hello; sleep 5"}});
    const auto wall = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started)
                          .count();
    const auto ran = aborted_milliseconds(result);
    if (!ran || *ran < 500 || *ran > 3500 || wall > 4000 || result.find("hello") == std::string::npos) {
        return fail_command(result, wall);
    }
    return EXIT_SUCCESS;
}

// The posted tool result has to survive a JSON round trip. Binary bytes do not.
static int assert_json_round_trip(const std::string &result) {
    const nlohmann::json body = {{"content", result}};
    const auto parsed = nlohmann::json::parse(body.dump());
    assertEquals(result, parsed.at("content").get<std::string>());
    return EXIT_SUCCESS;
}

/**
 * UTF-8 text is sent unchanged.
 */
static int test_tool_execute_command_keeps_utf8_text() {
    const auto tool = Tools::create_execute_command_tool();
    const auto result = tool.execute({{"command", "printf 'caf\\303\\251'"}});
    assertEquals(std::string("Exit code: 0\nOutput:\ncaf\xC3\xA9"), result);
    return assert_json_round_trip(result);
}

/**
 * Bytes that are not UTF-8 are escaped so the server can parse the tool result.
 */
static int test_tool_execute_command_escapes_binary() {
    const auto tool = Tools::create_execute_command_tool();
    const auto result = tool.execute({{"command", "printf '\\377\\376'"}});
    assertTrue(result.find("binary output (2 bytes)") != std::string::npos);
    assertTrue(result.find("\\xff\\xfe") != std::string::npos);
    assertTrue(result.find('\xff') == std::string::npos);
    return assert_json_round_trip(result);
}

/**
 * An embedded NUL is binary. JSON would otherwise carry a zero byte.
 */
static int test_tool_execute_command_escapes_nul() {
    const auto tool = Tools::create_execute_command_tool();
    const auto result = tool.execute({{"command", "printf 'a\\000b'"}});
    assertTrue(result.find("binary output (3 bytes)") != std::string::npos);
    assertTrue(result.find("a\\x00b") != std::string::npos);
    assertTrue(result.find('\0') == std::string::npos);
    return assert_json_round_trip(result);
}

/**
 * Run all execute command tests
 */
int test_tool_execute_command() {
    RUN_TEST(test_tool_execute_command_echo);
    RUN_TEST(test_tool_execute_command_echo_exitcode_1);
    RUN_TEST(test_tool_execute_command_error_missing_command);
    RUN_TEST(test_tool_execute_command_error_invalid_command);
    RUN_TEST(test_tool_execute_command_cancel_flag_idle);
    RUN_TEST(test_tool_execute_command_abort_before_start);
    RUN_TEST(test_tool_execute_command_abort_while_running);
    RUN_TEST(test_tool_execute_command_keeps_utf8_text);
    RUN_TEST(test_tool_execute_command_escapes_binary);
    RUN_TEST(test_tool_execute_command_escapes_nul);
    return EXIT_SUCCESS;
}

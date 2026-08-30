#include "common/agent.hpp"
#include "common/agent_backend.hpp"
#include "common/cli.hpp"
#include "common/color.hpp"
#include "common/context.hpp"
#include "common/net.hpp"
#include "common/protocol.hpp"
#include "common/tools.hpp"
#include "common/utf8.hpp"
#include "tests.hpp"

void test_tools() {
    std::cout << "[TEST] Running tool tests..." << std::endl;
    auto tools = get_registered_tools();
    assert(tools.size() >= 7);

    // Test write_file & read_file
    std::string test_path = "/tmp/local_ai_test_file.txt";
    nlohmann::json write_args = {
        {"path", test_path},
        {"content", "Line 1\nLine 2\nLine 3\n"}
    };
    std::string write_res = run_tool(tools, "write_file", write_args);
    assert(write_res.find("success") != std::string::npos);

    nlohmann::json read_args = {
        {"path", test_path},
        {"offset", 1},
        {"limit", 2}
    };
    std::string read_res = run_tool(tools, "read_file", read_args);
    assert(read_res.find("1: Line 1") != std::string::npos);
    assert(read_res.find("2: Line 2") != std::string::npos);
    assert(read_res.find("3: Line 3") == std::string::npos);

    // Test execute_command
    nlohmann::json cmd_args = {
        {"command", "echo 'hello localai'"}
    };
    std::string cmd_res = run_tool(tools, "execute_command", cmd_args);
    assert(cmd_res.find("Exit code: 0") != std::string::npos);
    assert(cmd_res.find("hello localai") != std::string::npos);

    // Test list_directory
    nlohmann::json list_args = {
        {"path", "/tmp"}
    };
    std::string list_res = run_tool(tools, "list_directory", list_args);
    assert(list_res.find("local_ai_test_file.txt") != std::string::npos);

    // Test file_search
    nlohmann::json search_args = {
        {"path", "/tmp"},
        {"pattern", "local_ai_test_file"}
    };
    std::string search_res = run_tool(tools, "file_search", search_args);
    assert(search_res.find("local_ai_test_file.txt") != std::string::npos);

    // Test search_text
    nlohmann::json text_search_args = {
        {"path", test_path},
        {"query", "Line 2"}
    };
    std::string text_search_res = run_tool(tools, "search_text", text_search_args);
    assert(text_search_res.find("2: Line 2") != std::string::npos);

    // Test tool aliases property and dispatch
    bool custom_executed = false;
    Tool custom_tool = {
        "custom_cmd",
        "A custom tool description",
        "arguments: none",
        [&](const nlohmann::json &) -> std::string {
            custom_executed = true;
            return "custom_tool_success";
        },
        {"alias_cmd1", "alias_cmd2"}
    };
    std::vector<Tool> test_tool_list = {custom_tool};
    assert(run_tool(test_tool_list, "alias_cmd1", {}) == "custom_tool_success");
    assert(custom_executed);
    custom_executed = false;
    assert(run_tool(test_tool_list, "alias_cmd2", {}) == "custom_tool_success");
    assert(custom_executed);
    assert(run_tool(test_tool_list, "unknown_cmd", {}).find("error: unknown tool") != std::string::npos);

    for (const auto & t : tools) {
        if (t.name == "search_text") {
            assert(!t.aliases.empty());
            assert(std::find(t.aliases.begin(), t.aliases.end(), "grep") != t.aliases.end());
        } else if (t.name == "web_search") {
            assert(!t.aliases.empty());
            assert(std::find(t.aliases.begin(), t.aliases.end(), "searxng_search") != t.aliases.end());
        }
    }

    // Clean up
    std::filesystem::remove(test_path);

    std::cout << "[TEST] Tool tests passed!" << std::endl;
}

void test_search_text() {
    std::cout << "[TEST] Running text search tool tests..." << std::endl;
    auto tools = get_registered_tools();

    std::string base_dir = "/tmp/local_ai_search_test_dir";
    std::filesystem::remove_all(base_dir);
    std::filesystem::create_directories(base_dir + "/nested");

    std::ofstream f1(base_dir + "/file1.txt");
    f1 << "Hello World\nThis is a text search test\nAnother line with HELLO\n";
    f1.close();

    std::ofstream f2(base_dir + "/file2.cpp");
    f2 << "#include <iostream>\nint calculate_total(int a, int b) {\n    return a + b;\n}\n";
    f2.close();

    std::ofstream f3(base_dir + "/file3.bin", std::ios::binary);
    char bin_data[] = {'\0', 'a', 'b', '\0', 'c', 'd'};
    f3.write(bin_data, sizeof(bin_data));
    f3.close();

    std::ofstream f4(base_dir + "/nested/file4.txt");
    f4 << "Nested search target: needle_in_haystack\nSecond line\n";
    f4.close();

    // 1. Case-insensitive substring search across directory
    {
        nlohmann::json args = {
            {"path", base_dir},
            {"query", "hello"}
        };
        std::string res = run_tool(tools, "search_text", args);
        assert(res.find("file1.txt:1: Hello World") != std::string::npos);
        assert(res.find("file1.txt:3: Another line with HELLO") != std::string::npos);
    }

    // 2. Case-sensitive substring search
    {
        nlohmann::json args = {
            {"path", base_dir},
            {"query", "HELLO"},
            {"case_sensitive", true}
        };
        std::string res = run_tool(tools, "search_text", args);
        assert(res.find("file1.txt:1: Hello World") == std::string::npos);
        assert(res.find("file1.txt:3: Another line with HELLO") != std::string::npos);
    }

    // 3. Regex search
    {
        nlohmann::json args = {
            {"path", base_dir},
            {"query", "calc.*total"},
            {"is_regex", true}
        };
        std::string res = run_tool(tools, "search_text", args);
        assert(res.find("file2.cpp:2: int calculate_total(int a, int b)") != std::string::npos);
    }

    // 4. File extension / pattern filter
    {
        nlohmann::json args = {
            {"path", base_dir},
            {"query", "int"},
            {"file_pattern", ".cpp"}
        };
        std::string res = run_tool(tools, "search_text", args);
        assert(res.find("file2.cpp") != std::string::npos);
        assert(res.find("file1.txt") == std::string::npos);
    }

    // 5. Nested directory search
    {
        nlohmann::json args = {
            {"path", base_dir},
            {"query", "needle_in_haystack"}
        };
        std::string res = run_tool(tools, "search_text", args);
        assert(res.find("file4.txt:1: Nested search target: needle_in_haystack") != std::string::npos);
    }

    // 6. Aliases: grep, find_text, search_in_files
    {
        nlohmann::json args = {
            {"path", base_dir},
            {"query", "needle_in_haystack"}
        };
        std::string res_grep = run_tool(tools, "grep", args);
        assert(res_grep.find("needle_in_haystack") != std::string::npos);

        std::string res_find = run_tool(tools, "find_text", args);
        assert(res_find.find("needle_in_haystack") != std::string::npos);

        std::string res_in_files = run_tool(tools, "search_in_files", args);
        assert(res_in_files.find("needle_in_haystack") != std::string::npos);
    }

    // 7. Missing query or non-existent path
    {
        nlohmann::json args_no_query = {
            {"path", base_dir}
        };
        std::string res_no_query = run_tool(tools, "search_text", args_no_query);
        assert(res_no_query.find("error: missing required string argument") != std::string::npos);

        nlohmann::json args_bad_path = {
            {"path", "/path/that/definitely/does/not/exist_12345"},
            {"query", "test"}
        };
        std::string res_bad_path = run_tool(tools, "search_text", args_bad_path);
        assert(res_bad_path.find("error: path") != std::string::npos);
    }

    // 8. Clean up
    std::filesystem::remove_all(base_dir);
    std::cout << "[TEST] Text search tool tests passed!" << std::endl;
}

void test_tool_call_parsing() {
    std::cout << "[TEST] Running tool call parsing tests..." << std::endl;
    std::string response = "I need to read the file.\n<tool_call>\n{\n  \"name\": \"read_file\",\n  \"arguments\": {\"path\": \"test.txt\"}\n}\n</tool_call>";
    std::string tool_name;
    nlohmann::json args;
    bool parsed = parse_tool_call(response, tool_name, args);
    (void)parsed;
    assert(parsed);
    assert(tool_name == "read_file");
    assert(args["path"] == "test.txt");

    // Tool call with empty think tags
    std::string think_response = "<think></think>\n<tool_call>\n{\n  \"name\": \"read_file\",\n  \"arguments\": {\"path\": \"think_test.txt\"}\n}\n</tool_call>";
    parsed = parse_tool_call(think_response, tool_name, args);
    assert(parsed);
    assert(tool_name == "read_file");
    assert(args["path"] == "think_test.txt");

    // Tool call after non-empty think block
    std::string think_reasoning_resp = "<think>Let me see if I should read the file.\nYes I should.</think>\n<tool_call>\n{\n  \"name\": \"read_file\",\n  \"arguments\": {\"path\": \"after_think.txt\"}\n}\n</tool_call>";
    parsed = parse_tool_call(think_reasoning_resp, tool_name, args);
    assert(parsed);
    assert(tool_name == "read_file");
    assert(args["path"] == "after_think.txt");

    // Raw JSON mentioned only inside <think> tag without <tool_call> marker should NOT trigger tool call
    std::string think_raw_json = "<think>I could perhaps call {\"name\": \"read_file\", \"arguments\": {\"path\": \"fake.txt\"}} but instead I will just answer.</think>Here is the answer.";
    parsed = parse_tool_call(think_raw_json, tool_name, args);
    assert(!parsed);

    std::string no_tool_response = "Here is the final answer for your query.";
    parsed = parse_tool_call(no_tool_response, tool_name, args);
    assert(!parsed);

    // Multi-tool calls parsing tests:
    // 1. Multiple <tool_call> tags
    std::string multi_tag_response = "I will perform two actions:\n"
                                     "<tool_call>\n{\n  \"name\": \"read_file\",\n  \"arguments\": {\"path\": \"file1.txt\"}\n}\n</tool_call>\n"
                                     "<tool_call>\n{\n  \"name\": \"read_file\",\n  \"arguments\": {\"path\": \"file2.txt\"}\n}\n</tool_call>";
    std::vector<ToolCall> tool_calls;
    bool multi_parsed = parse_tool_calls(multi_tag_response, tool_calls);
    assert(multi_parsed);
    assert(tool_calls.size() == 2);
    assert(tool_calls[0].name == "read_file");
    assert(tool_calls[0].arguments["path"] == "file1.txt");
    assert(tool_calls[1].name == "read_file");
    assert(tool_calls[1].arguments["path"] == "file2.txt");

    // 2. <tool_calls> tag with JSON array
    std::string array_in_tag_response = "<tool_calls>\n[\n"
                                        "  {\"name\": \"execute_command\", \"arguments\": {\"command\": \"ls -la\"}},\n"
                                        "  {\"name\": \"search_text\", \"arguments\": {\"path\": \".\", \"query\": \"main\"}}\n"
                                        "]\n</tool_calls>";
    tool_calls.clear();
    assert(parse_tool_calls(array_in_tag_response, tool_calls));
    assert(tool_calls.size() == 2);
    assert(tool_calls[0].name == "execute_command");
    assert(tool_calls[0].arguments["command"] == "ls -la");
    assert(tool_calls[1].name == "search_text");
    assert(tool_calls[1].arguments["query"] == "main");

    // 3. Raw JSON array without tags
    std::string raw_array_response = "[\n"
                                     "  {\"name\": \"web_fetch\", \"arguments\": {\"url\": \"http://example.com/1\"}},\n"
                                     "  {\"name\": \"web_fetch\", \"arguments\": {\"url\": \"http://example.com/2\"}}\n"
                                     "]";
    tool_calls.clear();
    assert(parse_tool_calls(raw_array_response, tool_calls));
    assert(tool_calls.size() == 2);
    assert(tool_calls[0].name == "web_fetch");
    assert(tool_calls[0].arguments["url"] == "http://example.com/1");
    assert(tool_calls[1].name == "web_fetch");
    assert(tool_calls[1].arguments["url"] == "http://example.com/2");

    // 4. Unclosed JSON object tests
    // 4a. Missing outer brace in <tool_call>
    std::string unclosed_outer = "<tool_call>\n{\n  \"name\": \"read_file\",\n  \"arguments\": {\"path\": \"unclosed1.txt\"}\n</tool_call>";
    parsed = parse_tool_call(unclosed_outer, tool_name, args);
    assert(parsed);
    assert(tool_name == "read_file");
    assert(args["path"] == "unclosed1.txt");

    // 4b. Missing inner and outer braces in <tool_call>
    std::string unclosed_inner_outer = "<tool_call>\n{\n  \"name\": \"read_file\",\n  \"arguments\": {\"path\": \"unclosed2.txt\"\n</tool_call>";
    parsed = parse_tool_call(unclosed_inner_outer, tool_name, args);
    assert(parsed);
    assert(tool_name == "read_file");
    assert(args["path"] == "unclosed2.txt");

    // 4c. Missing closing quote and braces in <tool_call>
    std::string unclosed_quote_braces = "<tool_call>\n{\n  \"name\": \"read_file\",\n  \"arguments\": {\"path\": \"unclosed3.txt\n</tool_call>";
    parsed = parse_tool_call(unclosed_quote_braces, tool_name, args);
    assert(parsed);
    assert(tool_name == "read_file");
    assert(args["path"] == "unclosed3.txt");

    // 4d. No closing </tool_call> tag AND unclosed JSON object
    std::string unclosed_tag_and_json = "I'll check the file:\n<tool_call>\n{\n  \"name\": \"read_file\",\n  \"arguments\": {\"path\": \"unclosed_notag.txt\"";
    parsed = parse_tool_call(unclosed_tag_and_json, tool_name, args);
    assert(parsed);
    assert(tool_name == "read_file");
    assert(args["path"] == "unclosed_notag.txt");

    // 4e. Trailing comma before closing braces
    std::string trailing_comma_json = "<tool_call>\n{\n  \"name\": \"read_file\",\n  \"arguments\": {\"path\": \"comma.txt\",},\n}\n</tool_call>";
    parsed = parse_tool_call(trailing_comma_json, tool_name, args);
    assert(parsed);
    assert(tool_name == "read_file");
    assert(args["path"] == "comma.txt");

    // 4f. Unclosed array in <tool_calls> tag
    std::string unclosed_array = "<tool_calls>\n[\n"
                                 "  {\"name\": \"execute_command\", \"arguments\": {\"command\": \"ls\"}},\n"
                                 "  {\"name\": \"read_file\", \"arguments\": {\"path\": \"arr.txt\"}\n"
                                 "</tool_calls>";
    tool_calls.clear();
    assert(parse_tool_calls(unclosed_array, tool_calls));
    assert(tool_calls.size() == 2);
    assert(tool_calls[0].name == "execute_command");
    assert(tool_calls[0].arguments["command"] == "ls");
    assert(tool_calls[1].name == "read_file");
    assert(tool_calls[1].arguments["path"] == "arr.txt");

    // 5. Invalid JSON in tool call tests (nlohmann parse error reporting)
    {
        std::string err;
        std::string broken_json = "<tool_call>\n{\n  \"name\": \"read_file\",\n  \"arguments\": {\n    \"path\": \n  }\n}\n</tool_call>";
        parsed = parse_tool_call(broken_json, tool_name, args, &err);
        assert(!parsed);
        assert(!err.empty());
        assert(err.find("parse_error") != std::string::npos || err.find("syntax error") != std::string::npos || err.find("[json.exception") != std::string::npos);

        std::string not_json = "<tool_call>\nread_file(path=\"test.txt\")\n</tool_call>";
        err.clear();
        parsed = parse_tool_call(not_json, tool_name, args, &err);
        assert(!parsed);
        assert(!err.empty());
        assert(err.find("parse_error") != std::string::npos || err.find("[json.exception") != std::string::npos);

        // Multiple tool calls tag with broken JSON
        std::string broken_array = "<tool_calls>\n[ {\"name\": \"read_file\", \"arguments\": } ]\n</tool_calls>";
        err.clear();
        tool_calls.clear();
        bool res = parse_tool_calls(broken_array, tool_calls, &err);
        assert(!res);
        assert(tool_calls.empty());
        assert(!err.empty());
        assert(err.find("parse_error") != std::string::npos || err.find("[json.exception") != std::string::npos);

        // Normal response without tool tags should have empty error
        std::string normal_msg = "Hello! I am ready to help you.";
        err.clear();
        tool_calls.clear();
        res = parse_tool_calls(normal_msg, tool_calls, &err);
        assert(!res);
        assert(tool_calls.empty());
        assert(err.empty());
    }

    // 6. Tool calls in the middle of text vs at the end of response
    {
        // 6a. Tool call in the middle of text with following content should NOT be interpreted
        std::string text_with_tool_call = "To read a file you can use <tool_call>{\"name\":\"read_file\",\"arguments\":{\"path\":\"test.txt\"}}</tool_call> in your instructions. Is there anything else you want to know?";
        std::string err;
        tool_name.clear();
        args.clear();
        parsed = parse_tool_call(text_with_tool_call, tool_name, args, &err);
        assert(!parsed);
        assert(tool_name.empty());
        assert(err.empty());

        tool_calls.clear();
        bool res = parse_tool_calls(text_with_tool_call, tool_calls, &err);
        assert(!res);
        assert(tool_calls.empty());
        assert(err.empty());

        // 6b. Broken JSON in tool call in the middle of text should NOT produce a parse error
        std::string broken_in_text = "Here is an example: <tool_call>{\"name\":\"read_file\", \"arguments\": }</tool_call> and here is how to fix it.";
        err.clear();
        tool_calls.clear();
        res = parse_tool_calls(broken_in_text, tool_calls, &err);
        assert(!res);
        assert(tool_calls.empty());
        assert(err.empty());

        // 6c. Tool call tag without closing tag in the middle of text
        std::string unclosed_in_text = "You can write <tool_call> tag to invoke tools in the terminal.";
        err.clear();
        tool_calls.clear();
        res = parse_tool_calls(unclosed_in_text, tool_calls, &err);
        assert(!res);
        assert(tool_calls.empty());
        assert(err.empty());

        // 6d. Text before tool call, but tool call is at the end of response -> MUST be parsed
        std::string text_then_tool = "I will now proceed with reading the file:\n<tool_call>\n{\n  \"name\": \"read_file\",\n  \"arguments\": {\"path\": \"actual_file.txt\"}\n}\n</tool_call>\n\n  ";
        err.clear();
        tool_name.clear();
        args.clear();
        parsed = parse_tool_call(text_then_tool, tool_name, args, &err);
        assert(parsed);
        assert(tool_name == "read_file");
        assert(args["path"] == "actual_file.txt");
        assert(err.empty());

        // 6e. Tool call in middle of text (ignored) and real tool call at the end (interpreted)
        std::string mixed_response = "For example: <tool_call>{\"name\":\"fake_tool\"}</tool_call>. Now doing the actual call:\n<tool_call>{\"name\":\"real_tool\",\"arguments\":{\"target\":\"main\"}}</tool_call>";
        err.clear();
        tool_calls.clear();
        res = parse_tool_calls(mixed_response, tool_calls, &err);
        assert(res);
        assert(tool_calls.size() == 1);
        assert(tool_calls[0].name == "real_tool");
        assert(tool_calls[0].arguments["target"] == "main");

        // 6f. Raw JSON in middle of text should not be interpreted as tool call
        std::string raw_json_in_text = "The returned data was {\"name\": \"read_file\", \"arguments\": {\"path\": \"test.txt\"}} as shown above.";
        tool_calls.clear();
        res = parse_tool_calls(raw_json_in_text, tool_calls);
        assert(!res);
        assert(tool_calls.empty());
    }

    std::cout << "[TEST] Tool call parsing tests passed!" << std::endl;
}

void test_protocol_and_sockets() {
    std::cout << "[TEST] Running socket & protocol tests..." << std::endl;
    int test_port = 19876;
    std::string host = "127.0.0.1";

    TcpServer server;
    std::string err;
    bool listened = server.listen(host, test_port, err);
    (void)listened;
    assert(listened);

    std::thread server_thread([&]() {
        auto client_sock = server.accept();
        assert(client_sock && client_sock->is_valid());

        nlohmann::json req;
        bool read_ok = client_sock->read_json(req);
        (void)read_ok;
        assert(read_ok);
        assert(req["type"] == "ping");

        nlohmann::json pong = {{"type", "pong"}, {"status", "ok"}, {"n_ctx", 4096}, {"used_ctx", 120}};
        client_sock->send_json(pong);

        // Expect a context query
        read_ok = client_sock->read_json(req);
        assert(read_ok);
        assert(req["type"] == "context");
        nlohmann::json ctx_resp = {{"type", "context"}, {"n_ctx", 4096}, {"used_ctx", 120}};
        client_sock->send_json(ctx_resp);

        // Expect a chat message
        read_ok = client_sock->read_json(req);
        assert(read_ok);
        assert(req["type"] == "chat");
        auto msgs = Protocol::parse_messages(req["messages"]);
        assert(msgs.size() == 2);
        assert(msgs[0].role == "system");
        assert(msgs[1].role == "user");

        // Stream 2 tokens then done with context metrics
        client_sock->send_json({{"type", "token"}, {"piece", "Hello"}});
        client_sock->send_json({{"type", "token"}, {"piece", " world"}});
        client_sock->send_json({{"type", "done"}, {"response", "Hello world"}, {"n_ctx", 4096}, {"used_ctx", 135}});
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    std::string conn_err;
    auto client = TcpClient::connect(host, test_port, conn_err);
    assert(client && client->is_valid());

    client->send_json({{"type", "ping"}});
    nlohmann::json pong_resp;
    bool read_pong = client->read_json(pong_resp);
    (void)read_pong;
    assert(read_pong);
    assert(pong_resp["type"] == "pong");
    assert(pong_resp["status"] == "ok");
    assert(pong_resp["n_ctx"] == 4096);
    assert(pong_resp["used_ctx"] == 120);

    // Test context request
    client->send_json({{"type", "context"}});
    nlohmann::json ctx_resp;
    assert(client->read_json(ctx_resp));
    assert(ctx_resp["type"] == "context");
    assert(ctx_resp["n_ctx"] == 4096);
    assert(ctx_resp["used_ctx"] == 120);

    std::vector<Protocol::ChatMessage> msgs = {
        {"system", "You are a helpful assistant."},
        {"user", "Say hello."}
    };
    client->send_json({
        {"type", "chat"},
        {"messages", Protocol::messages_to_json(msgs)},
        {"temperature", 0.7f}
    });

    nlohmann::json t1, t2, done;
    assert(client->read_json(t1) && t1["type"] == "token" && t1["piece"] == "Hello");
    assert(client->read_json(t2) && t2["type"] == "token" && t2["piece"] == " world");
    assert(client->read_json(done) && done["type"] == "done" && done["response"] == "Hello world");
    assert(done["n_ctx"] == 4096);
    assert(done["used_ctx"] == 135);

    // Test socket timeout helper
    assert(client->set_timeout(1));

    server_thread.join();
    server.close();
    client->close();
    std::cout << "[TEST] Socket & protocol tests passed!" << std::endl;
}

void test_utf8_streaming() {
    std::cout << "[TEST] Running UTF-8 streaming tests..." << std::endl;

    // Test get_complete_utf8_prefix_len
    assert(Utf8Util::get_complete_utf8_prefix_len("") == 0);
    assert(Utf8Util::get_complete_utf8_prefix_len("hello world") == 11);

    // 2-byte UTF-8 character: "ö" = \xC3\xB6
    std::string incomplete_2byte = "test\xC3";
    assert(Utf8Util::get_complete_utf8_prefix_len(incomplete_2byte) == 4);
    std::string complete_2byte = "test\xC3\xB6";
    assert(Utf8Util::get_complete_utf8_prefix_len(complete_2byte) == 6);

    // 3-byte UTF-8 character: "’" = \xE2\x80\x99
    std::string incomplete_3byte_1 = "quote \xE2";
    assert(Utf8Util::get_complete_utf8_prefix_len(incomplete_3byte_1) == 6);
    std::string incomplete_3byte_2 = "quote \xE2\x80";
    assert(Utf8Util::get_complete_utf8_prefix_len(incomplete_3byte_2) == 6);
    std::string complete_3byte = "quote \xE2\x80\x99";
    assert(Utf8Util::get_complete_utf8_prefix_len(complete_3byte) == 9);

    // 4-byte UTF-8 character: "😀" = \xF0\x9F\x98\x80
    std::string incomplete_4byte = "emoji \xF0\x9F\x98";
    assert(Utf8Util::get_complete_utf8_prefix_len(incomplete_4byte) == 6);
    std::string complete_4byte = "emoji \xF0\x9F\x98\x80";
    assert(Utf8Util::get_complete_utf8_prefix_len(complete_4byte) == 10);

    // Test Utf8Util::StreamBuffer split across tokens
    Utf8Util::StreamBuffer buf;
    // Token 1 has "Hello \xE2\x80" (incomplete smart quote)
    std::string out1 = buf.process("Hello \xE2\x80");
    assert(out1 == "Hello ");

    // Token 2 has "\x99 world!" (continuation byte finishing smart quote)
    std::string out2 = buf.process("\x99 world!");
    assert(out2 == "\xE2\x80\x99 world!");

    // Flush should be empty
    std::string remaining = buf.flush();
    assert(remaining.empty());

    // Test emoji split byte by byte
    buf.reset();
    assert(buf.process("\xF0") == "");
    assert(buf.process("\x9F") == "");
    assert(buf.process("\x98") == "");
    assert(buf.process("\x80") == "\xF0\x9F\x98\x80");

    // Test flush with leftover
    buf.reset();
    buf.process("abc\xE2");
    assert(buf.flush() == "\xE2");

    std::cout << "[TEST] UTF-8 streaming tests passed!" << std::endl;
}

void test_utf8_json_resilience() {
    std::cout << "[TEST] Running UTF-8 JSON resilience tests..." << std::endl;
    int test_port = 19877;
    std::string host = "127.0.0.1";

    TcpServer server;
    std::string err;
    bool listened = server.listen(host, test_port, err);
    (void)listened;
    assert(listened);

    std::thread server_thread([&]() {
        auto client_sock = server.accept();
        assert(client_sock && client_sock->is_valid());

        // Send a JSON packet containing an invalid/incomplete UTF-8 byte (0x91 as reported)
        // This should serialize cleanly using replacement without throwing type_error.316
        std::string raw_invalid = "invalid \x91 byte";
        nlohmann::json test_json = {{"type", "token"}, {"piece", raw_invalid}};
        bool sent = client_sock->send_json(test_json);
        (void)sent;
        assert(sent);
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    std::string conn_err;
    auto client = TcpClient::connect(host, test_port, conn_err);
    assert(client && client->is_valid());

    nlohmann::json received;
    bool read_ok = client->read_json(received);
    (void)read_ok;
    assert(read_ok);
    assert(received["type"] == "token");
    assert(received.contains("piece"));

    server_thread.join();
    server.close();
    client->close();
    std::cout << "[TEST] UTF-8 JSON resilience tests passed!" << std::endl;
}

void test_context_management() {
    std::cout << "[TEST] Running context management tests..." << std::endl;

    // Test threshold and percentage
    assert(!Context::should_compact(0, 4096));
    assert(!Context::should_compact(1000, 4096, 0.75f));
    assert(Context::should_compact(3072, 4096, 0.75f));
    assert(Context::should_compact(3500, 4096, 0.75f));

    float pct = Context::get_usage_percentage(2048, 4096);
    assert(std::abs(pct - 50.0f) < 0.001f);

    std::string bar = Context::format_progress_bar(2048, 4096, 10);
    assert(bar == "[=====-----]");

    // Test server-side context formatting and logging helpers
    std::string summary = Context::format_context_usage_summary(1024, 4096);
    assert(summary == "1024 / 4096 tokens (25.0%)");

    std::string client_log_default = Context::format_client_context_log("127.0.0.1", 54321, 1024, 4096);
    assert(client_log_default == "[server] Client 127.0.0.1:54321 context: 1024 / 4096 tokens (25.0%)");

    std::string client_log_chat = Context::format_client_context_log("192.168.1.50", 8080, 2048, 4096, "chat");
    assert(client_log_chat == "[server] Client 192.168.1.50:8080 chat context: 2048 / 4096 tokens (50.0%)");

    std::string client_log_gen = Context::format_client_context_log("10.0.0.1", 9000, 3072, 4096, "generate");
    assert(client_log_gen == "[server] Client 10.0.0.1:9000 generate context: 3072 / 4096 tokens (75.0%)");

    std::cout << "[TEST] Context management tests passed!" << std::endl;
}

void test_context_compaction() {
    std::cout << "[TEST] Running context compaction tests..." << std::endl;

    std::vector<Protocol::ChatMessage> msgs = {
        {"system", "System instruction prompt"},
        {"user", "Hello, please list files."},
        {"assistant", "<tool_call>{\"name\":\"list_directory\"}</tool_call>"},
        {"tool", "<tool_response>file1.txt, file2.txt</tool_response>"},
        {"assistant", "I see file1.txt and file2.txt."},
        {"user", "Now read file1.txt"},
        {"assistant", "<tool_call>{\"name\":\"read_file\"}</tool_call>"},
        {"tool", "<tool_response>content of file 1</tool_response>"},
        {"assistant", "Here is the content of file 1."}
    };

    // Test summary prompt generation
    std::string prompt = Context::build_summary_prompt(msgs);
    assert(prompt.find("System instruction prompt") != std::string::npos);
    assert(prompt.find("file1.txt") != std::string::npos);

    // Test fallback summary
    std::string fallback = Context::create_fallback_summary(msgs);
    assert(fallback.find("9 messages") != std::string::npos);

    // Test compact_messages with keep_recent = 2
    std::string summary_text = "User listed files and read file1.txt successfully.";
    bool compacted = Context::compact_messages(msgs, summary_text, 2);
    assert(compacted);
    // Result should be:
    // [0]: system prompt
    // [1]: user with summary
    // [2]: assistant ack
    // [3]: tool (second to last message)
    // [4]: assistant (last message)
    assert(msgs.size() == 5);
    assert(msgs[0].role == "system");
    assert(msgs[0].content == "System instruction prompt");
    assert(msgs[1].role == "user");
    assert(msgs[1].content.find(summary_text) != std::string::npos);
    assert(msgs[2].role == "assistant");
    assert(msgs[3].role == "tool");
    assert(msgs[3].content == "<tool_response>content of file 1</tool_response>");
    assert(msgs[4].role == "assistant");
    assert(msgs[4].content == "Here is the content of file 1.");

    // Test with minimal messages (<= 2)
    std::vector<Protocol::ChatMessage> small_msgs = {
        {"system", "sys"},
        {"user", "hi"}
    };
    assert(!Context::compact_messages(small_msgs, "summary", 2));
    assert(small_msgs.size() == 2);

    std::cout << "[TEST] Context compaction tests passed!" << std::endl;
}

void test_subagents() {
    std::cout << "[TEST] Running sub-agent tests..." << std::endl;

    // 1. Test base tools vs registered tools with sub-agents disabled/enabled
    auto base_tools = get_base_tools();
    for (const auto & t : base_tools) {
        assert(t.name != "sub_agent");
    }

    auto default_tools = get_registered_tools(false);
    assert(default_tools.size() == base_tools.size());
    for (const auto & t : default_tools) {
        assert(t.name != "sub_agent");
    }

    // Default registered tools should have subagents enabled by default
    auto default_with_subagents = get_registered_tools();
    assert(default_with_subagents.size() == base_tools.size() + 1);

    std::string received_task;
    auto mock_runner = [&](std::string_view task) -> std::string {
        received_task = std::string(task);
        return "Sub-agent result for: " + std::string(task);
    };

    auto enabled_tools = get_registered_tools(true, mock_runner);
    assert(enabled_tools.size() == base_tools.size() + 1);
    bool found_subagent = false;
    for (const auto & t : enabled_tools) {
        if (t.name == "sub_agent") {
            found_subagent = true;
            assert(!t.description.empty());
            assert(!t.schema_doc.empty());
            assert(!t.aliases.empty());
            assert(std::find(t.aliases.begin(), t.aliases.end(), "subagent") != t.aliases.end());
        }
    }
    assert(found_subagent);

    // 2. Test system prompt generation with sub-agents enabled vs disabled
    std::string prompt_disabled = build_system_prompt(default_tools);
    assert(prompt_disabled.find("sub_agent") == std::string::npos);
    assert(prompt_disabled.find("Sub-Agent Task Planning") == std::string::npos);

    std::string prompt_enabled = build_system_prompt(enabled_tools);
    assert(prompt_enabled.find("sub_agent") != std::string::npos);
    assert(prompt_enabled.find("Sub-Agent Task Planning") != std::string::npos);
    assert(prompt_enabled.find("Context Optimization") != std::string::npos);
    assert(prompt_enabled.find("Reusing Sub-Agent Results") != std::string::npos);
    assert(prompt_enabled.find("Planning Phase") != std::string::npos);
    assert(prompt_enabled.find("decompose the problem into distinct") != std::string::npos);
    assert(prompt_enabled.find("Every sub-agent's response and findings can also directly result in executing tools") != std::string::npos);
    assert(prompt_enabled.find("If multiple tasks are passed, they will all be executed in sequential, controlled order") != std::string::npos);

    // 3. Test running sub_agent tool with various argument formats
    nlohmann::json args1 = {{"task", "Investigate issue #42"}};
    std::string res1 = run_tool(enabled_tools, "sub_agent", args1);
    assert(res1 == "Sub-agent result for: Investigate issue #42");
    assert(received_task == "Investigate issue #42");

    // Test with alias "subagent"
    nlohmann::json args_alias = {{"task", "Run search"}};
    std::string res_alias = run_tool(enabled_tools, "subagent", args_alias);
    assert(res_alias == "Sub-agent result for: Run search");

    // Test with alias "delegate_task" and argument "prompt"
    nlohmann::json args_prompt = {{"prompt", "Delegate work"}};
    std::string res_prompt = run_tool(enabled_tools, "delegate_task", args_prompt);
    assert(res_prompt == "Sub-agent result for: Delegate work");

    // Test with alias "run_task" and argument "subtask"
    nlohmann::json args_subtask = {{"subtask", "Step 1: Scan files"}};
    std::string res_subtask = run_tool(enabled_tools, "run_task", args_subtask);
    assert(res_subtask == "Sub-agent result for: Step 1: Scan files");

    // Test with alias "sub_task" and argument "sub_task"
    nlohmann::json args_sub_task = {{"sub_task", "Step 2: Edit code"}};
    std::string res_sub_task = run_tool(enabled_tools, "sub_task", args_sub_task);
    assert(res_sub_task == "Sub-agent result for: Step 2: Edit code");

    // Test missing argument
    nlohmann::json args_empty = nlohmann::json::object();
    std::string res_empty = run_tool(enabled_tools, "sub_agent", args_empty);
    assert(res_empty.find("error") != std::string::npos);

    // Test unconfigured runner
    auto unconfigured_tools = get_registered_tools(true, nullptr);
    std::string res_unconfigured = run_tool(unconfigured_tools, "sub_agent", args1);
    assert(res_unconfigured.find("error") != std::string::npos);

    // 4. Test tool call parsing for sub-agent
    std::string tool_call_str = "<tool_call>\n{\n  \"name\": \"sub_agent\",\n  \"arguments\": {\"task\": \"Analyze codebase\"}\n}\n</tool_call>";
    std::string parsed_name;
    nlohmann::json parsed_args;
    bool parsed = parse_tool_call(tool_call_str, parsed_name, parsed_args);
    assert(parsed);
    assert(parsed_name == "sub_agent");
    assert(parsed_args["task"] == "Analyze codebase");

    // 5. Test isolated context: executing sub-agent tool does not modify parent messages
    std::vector<Protocol::ChatMessage> parent_messages = {
        {"system", prompt_enabled},
        {"user", "Please break down the refactoring into tasks and solve it."}
    };
    std::string sub_result = run_tool(enabled_tools, parsed_name, parsed_args);
    assert(parent_messages.size() == 2); // Parent messages untouched during tool call
    parent_messages.push_back({"assistant", "Planning: Decomposing into Task 1 (Analyze) and Task 2 (Refactor).\n" + tool_call_str});
    parent_messages.push_back({"tool", "<tool_response>\n" + sub_result + "\n</tool_response>"});
    assert(parent_messages.size() == 4);

    // Test second sub-agent execution in sequence (Task 2)
    std::string tool_call_step2 = "<tool_call>\n{\n  \"name\": \"sub_agent\",\n  \"arguments\": {\"task\": \"Implement refactor\"}\n}\n</tool_call>";
    std::string parsed_name2;
    nlohmann::json parsed_args2;
    bool parsed2 = parse_tool_call(tool_call_step2, parsed_name2, parsed_args2);
    assert(parsed2);
    std::string sub_result2 = run_tool(enabled_tools, parsed_name2, parsed_args2);
    assert(sub_result2 == "Sub-agent result for: Implement refactor");
    parent_messages.push_back({"assistant", tool_call_step2});
    parent_messages.push_back({"tool", "<tool_response>\n" + sub_result2 + "\n</tool_response>"});
    assert(parent_messages.size() == 6);

    // 6. Test that a sub-agent's answer can directly result in tools being executed (e.g., file operations, commands)
    std::string test_file = "test_subagent_tool_exec_tmp.txt";
    auto file_managing_runner = [&](std::string_view task) -> std::string {
        if (task.find("analyze") != std::string_view::npos) {
            return "Analysis complete. Suggested action: write file '" + test_file + "' with content 'generated_by_subagent_task'.";
        }
        return "Sub-agent completed task: " + std::string(task);
    };

    auto tools_with_file_runner = get_registered_tools(true, file_managing_runner);

    // Step A: Sub-agent runs analysis
    std::string analyze_call = "<tool_call>\n{\n  \"name\": \"sub_agent\",\n  \"arguments\": {\"task\": \"analyze project requirements\"}\n}\n</tool_call>";
    std::string a_name;
    nlohmann::json a_args;
    assert(parse_tool_call(analyze_call, a_name, a_args));
    std::string analyze_res = run_tool(tools_with_file_runner, a_name, a_args);
    assert(analyze_res.find("write file '" + test_file + "'") != std::string::npos);

    // Step B: Main agent executes write_file tool as a direct result of sub-agent's response
    std::string write_call = "<tool_call>\n{\n  \"name\": \"write_file\",\n  \"arguments\": {\"path\": \"" + test_file + "\", \"content\": \"generated_by_subagent_task\"}\n}\n</tool_call>";
    std::string w_name;
    nlohmann::json w_args;
    assert(parse_tool_call(write_call, w_name, w_args));
    std::string write_res = run_tool(tools_with_file_runner, w_name, w_args);
    assert(write_res.find("success: wrote") != std::string::npos);

    // Step C: Main agent reads the file to verify
    std::string read_call = "<tool_call>\n{\n  \"name\": \"read_file\",\n  \"arguments\": {\"path\": \"" + test_file + "\"}\n}\n</tool_call>";
    std::string r_name;
    nlohmann::json r_args;
    assert(parse_tool_call(read_call, r_name, r_args));
    std::string read_res = run_tool(tools_with_file_runner, r_name, r_args);
    assert(read_res.find("generated_by_subagent_task") != std::string::npos);

    // Clean up temporary test file
    std::error_code ec_clean;
    std::filesystem::remove(test_file, ec_clean);

    // 7. Test multiple tasks execution in controlled sequence
    std::vector<std::string> executed_tasks_sequence;
    auto seq_runner = [&](std::string_view t) -> std::string {
        executed_tasks_sequence.push_back(std::string(t));
        return "Completed: " + std::string(t);
    };
    auto seq_tools = get_registered_tools(true, seq_runner);

    // 7a. Test "tasks" array of strings
    executed_tasks_sequence.clear();
    nlohmann::json multi_tasks_args = {
        {"tasks", {"Task 1: Search repo", "Task 2: Read headers", "Task 3: Refactor code"}}
    };
    std::string multi_res = run_tool(seq_tools, "sub_agent", multi_tasks_args);
    assert(executed_tasks_sequence.size() == 3);
    assert(executed_tasks_sequence[0] == "Task 1: Search repo");
    assert(executed_tasks_sequence[1] == "Task 2: Read headers");
    assert(executed_tasks_sequence[2] == "Task 3: Refactor code");
    assert(multi_res.find("### Task 1/3: Task 1: Search repo\nCompleted: Task 1: Search repo") != std::string::npos);
    assert(multi_res.find("### Task 2/3: Task 2: Read headers\nCompleted: Task 2: Read headers") != std::string::npos);
    assert(multi_res.find("### Task 3/3: Task 3: Refactor code\nCompleted: Task 3: Refactor code") != std::string::npos);

    // 7b. Test "subtasks" array of objects
    executed_tasks_sequence.clear();
    nlohmann::json subtasks_obj_args = {
        {"subtasks", {
            {{"task", "Phase 1: Build client"}},
            {{"prompt", "Phase 2: Build server"}}
        }}
    };
    std::string subtasks_res = run_tool(seq_tools, "sub_agent", subtasks_obj_args);
    assert(executed_tasks_sequence.size() == 2);
    assert(executed_tasks_sequence[0] == "Phase 1: Build client");
    assert(executed_tasks_sequence[1] == "Phase 2: Build server");
    assert(subtasks_res.find("### Task 1/2: Phase 1: Build client") != std::string::npos);
    assert(subtasks_res.find("### Task 2/2: Phase 2: Build server") != std::string::npos);

    // 7c. Test raw array of tasks
    executed_tasks_sequence.clear();
    nlohmann::json raw_array_tasks = {"Step A", "Step B"};
    std::string raw_res = run_tool(seq_tools, "sub_agent", raw_array_tasks);
    assert(executed_tasks_sequence.size() == 2);
    assert(executed_tasks_sequence[0] == "Step A");
    assert(executed_tasks_sequence[1] == "Step B");

    std::cout << "[TEST] Sub-agent tests passed!" << std::endl;
}

void test_tool_approval() {
    std::cout << "[TEST] Running tool approval tests..." << std::endl;

    // 1. Test parse_tool_approval_input with various valid/invalid inputs
    assert(parse_tool_approval_input("ja") == ToolApprovalParseResult::ALLOW);
    assert(parse_tool_approval_input("JA") == ToolApprovalParseResult::ALLOW);
    assert(parse_tool_approval_input("  j  ") == ToolApprovalParseResult::ALLOW);
    assert(parse_tool_approval_input("yes") == ToolApprovalParseResult::ALLOW);
    assert(parse_tool_approval_input("YES") == ToolApprovalParseResult::ALLOW);
    assert(parse_tool_approval_input("y") == ToolApprovalParseResult::ALLOW);
    assert(parse_tool_approval_input("1") == ToolApprovalParseResult::ALLOW);

    assert(parse_tool_approval_input("nej") == ToolApprovalParseResult::DENY);
    assert(parse_tool_approval_input("NEJ") == ToolApprovalParseResult::DENY);
    assert(parse_tool_approval_input("  n  ") == ToolApprovalParseResult::DENY);
    assert(parse_tool_approval_input("no") == ToolApprovalParseResult::DENY);
    assert(parse_tool_approval_input("NO") == ToolApprovalParseResult::DENY);
    assert(parse_tool_approval_input("0") == ToolApprovalParseResult::DENY);

    assert(parse_tool_approval_input("alltid ja") == ToolApprovalParseResult::ALWAYS);
    assert(parse_tool_approval_input("ALLTID JA") == ToolApprovalParseResult::ALWAYS);
    assert(parse_tool_approval_input("  alltid ja  ") == ToolApprovalParseResult::ALWAYS);
    assert(parse_tool_approval_input("alltid") == ToolApprovalParseResult::ALWAYS);
    assert(parse_tool_approval_input("a") == ToolApprovalParseResult::ALWAYS);
    assert(parse_tool_approval_input("A") == ToolApprovalParseResult::ALWAYS);
    assert(parse_tool_approval_input("always ja") == ToolApprovalParseResult::ALWAYS);
    assert(parse_tool_approval_input("always yes") == ToolApprovalParseResult::ALWAYS);
    assert(parse_tool_approval_input("always") == ToolApprovalParseResult::ALWAYS);

    assert(parse_tool_approval_input("") == ToolApprovalParseResult::INVALID);
    assert(parse_tool_approval_input("   ") == ToolApprovalParseResult::INVALID);
    assert(parse_tool_approval_input("kanske") == ToolApprovalParseResult::INVALID);
    assert(parse_tool_approval_input("maybe") == ToolApprovalParseResult::INVALID);
    assert(parse_tool_approval_input("cancel") == ToolApprovalParseResult::INVALID);

    // 2. Test prompt_tool_approval with simulated user inputs
    nlohmann::json dummy_args = {{"command", "ls -la"}};

    {
        std::istringstream in("ja\n");
        std::ostringstream out;
        assert(prompt_tool_approval("execute_command", dummy_args, in, out) == ToolApproval::ALLOW);
        assert(out.str().find("execute_command") != std::string::npos);
        assert(out.str().find("ls -la") != std::string::npos);
    }

    {
        std::istringstream in("nej\n");
        std::ostringstream out;
        assert(prompt_tool_approval("execute_command", dummy_args, in, out) == ToolApproval::DENY);
    }

    {
        std::istringstream in("alltid ja\n");
        std::ostringstream out;
        assert(prompt_tool_approval("execute_command", dummy_args, in, out) == ToolApproval::ALWAYS);
    }

    {
        std::istringstream in("ogiltigt svar\nja\n");
        std::ostringstream out;
        assert(prompt_tool_approval("execute_command", dummy_args, in, out) == ToolApproval::ALLOW);
        assert(out.str().find("Ogiltigt val") != std::string::npos);
    }

    {
        std::istringstream in(""); // EOF immediately
        std::ostringstream out;
        assert(prompt_tool_approval("execute_command", dummy_args, in, out) == ToolApproval::DENY);
    }

    // 3. Test approval workflow simulation with auto_approve transition
    bool auto_approve = false;
    int execution_count = 0;
    auto execute_with_approval = [&](const std::string & tool_name,
                                     const nlohmann::json & args,
                                     std::istream & in,
                                     std::ostream & out) -> std::string {
        if (!auto_approve) {
            ToolApproval app = prompt_tool_approval(tool_name, args, in, out);
            if (app == ToolApproval::ALWAYS) {
                auto_approve = true;
            } else if (app == ToolApproval::DENY) {
                return "<tool_response>\nerror: tool execution was denied by the user.\n</tool_response>";
            }
        }
        execution_count++;
        return "<tool_response>\nexecuted " + tool_name + "\n</tool_response>";
    };

    // Step a: Denied tool
    {
        std::istringstream in("nej\n");
        std::ostringstream out;
        std::string res = execute_with_approval("write_file", {{"path", "test.txt"}}, in, out);
        assert(res.find("denied by the user") != std::string::npos);
        assert(execution_count == 0);
        assert(!auto_approve);
    }

    // Step b: Single approved tool
    {
        std::istringstream in("ja\n");
        std::ostringstream out;
        std::string res = execute_with_approval("read_file", {{"path", "test.txt"}}, in, out);
        assert(res.find("executed read_file") != std::string::npos);
        assert(execution_count == 1);
        assert(!auto_approve);
    }

    // Step c: Always allow tool
    {
        std::istringstream in("alltid ja\n");
        std::ostringstream out;
        std::string res = execute_with_approval("execute_command", {{"command", "date"}}, in, out);
        assert(res.find("executed execute_command") != std::string::npos);
        assert(execution_count == 2);
        assert(auto_approve == true);
    }

    // Step d: Next call runs automatically without prompt input
    {
        std::istringstream in(""); // Empty input should not matter since auto_approve is true
        std::ostringstream out;
        std::string res = execute_with_approval("web_search", {{"query", "news"}}, in, out);
        assert(res.find("executed web_search") != std::string::npos);
        assert(execution_count == 3);
        assert(out.str().empty()); // No prompt printed
    }

    std::cout << "[TEST] Tool approval tests passed!" << std::endl;
}

void test_ai_instructions() {
    std::cout << "[TEST] Running AI instructions (AI_INSTRUCTIONS.md) tests..." << std::endl;

    std::string test_dir = "test_ai_instructions_tmp_" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count());
    std::filesystem::create_directories(test_dir);

    // 1. In empty dir, load_ai_instructions should return empty string
    assert(load_ai_instructions(test_dir).empty());

    // 2. In empty dir, build_system_prompt should not contain "## Project Instructions (AI_INSTRUCTIONS.md):"
    auto base_tools = get_base_tools();
    std::string prompt1 = build_system_prompt(base_tools, "Some custom instruction", test_dir);
    assert(prompt1.find("## Project Instructions (AI_INSTRUCTIONS.md):") == std::string::npos);
    assert(prompt1.find("Some custom instruction") != std::string::npos);

    // 3. Create AI_INSTRUCTIONS.md
    std::string instructions_content = "# Project Guidelines\n- Always write unit tests.\n- Use modern C++20 standard.";
    {
        std::ofstream ofs(test_dir + "/AI_INSTRUCTIONS.md");
        ofs << instructions_content << "\n\n";
    }

    std::string loaded = load_ai_instructions(test_dir);
    assert(loaded == instructions_content);

    // 4. Verify that build_system_prompt includes AI_INSTRUCTIONS.md
    std::string prompt2 = build_system_prompt(base_tools, "User custom flag", test_dir);
    assert(prompt2.find("## Project Instructions (AI_INSTRUCTIONS.md):") != std::string::npos);
    assert(prompt2.find("Always write unit tests.") != std::string::npos);
    assert(prompt2.find("User custom flag") != std::string::npos);

    // 5. Test fallback to .github/copilot-instructions.md if AI_INSTRUCTIONS.md is not present
    std::filesystem::remove(test_dir + "/AI_INSTRUCTIONS.md");
    std::filesystem::create_directories(test_dir + "/.github");
    {
        std::ofstream ofs(test_dir + "/.github/copilot-instructions.md");
        ofs << "Follow GitHub Copilot instructions.";
    }

    std::string copilot_loaded = load_ai_instructions(test_dir);
    assert(copilot_loaded == "Follow GitHub Copilot instructions.");

    std::string prompt3 = build_system_prompt(base_tools, "", test_dir);
    assert(prompt3.find("## Project Instructions (AI_INSTRUCTIONS.md):") != std::string::npos);
    assert(prompt3.find("Follow GitHub Copilot instructions.") != std::string::npos);

    // 6. If both exist, AI_INSTRUCTIONS.md takes precedence
    {
        std::ofstream ofs(test_dir + "/AI_INSTRUCTIONS.md");
        ofs << "Priority AI_INSTRUCTIONS";
    }
    assert(load_ai_instructions(test_dir) == "Priority AI_INSTRUCTIONS");

    // Clean up
    std::error_code ec;
    std::filesystem::remove_all(test_dir, ec);

    std::cout << "[TEST] AI instructions tests passed!" << std::endl;
}

void test_command_execution() {
    std::cout << "[TEST] Running command execution tests..." << std::endl;

    auto tools = get_registered_tools();

    // 2. Test `/sh`, `/run`, `/cmd` prefixes
    {
        for (const char * prefix : {"/sh ", "/run ", "/cmd "}) {
            std::string user_input = std::string(prefix) + "echo 'prefix test'";
            size_t space_pos = user_input.find(' ');
            std::string cmd = user_input.substr(space_pos + 1);
            nlohmann::json args = {{"command", cmd}};
            std::string res = run_tool(tools, "execute_command", args);
            assert(res.find("prefix test") != std::string::npos);
        }
    }

    // 3. Test end-to-end single command turn via socket mock
    {
        int test_port = 19888;
        std::string host = "127.0.0.1";
        TcpServer server;
        std::string err;
        bool listened = server.listen(host, test_port, err);
        assert(listened);

        std::thread server_thread([&]() {
            auto client_sock = server.accept();
            assert(client_sock && client_sock->is_valid());

            // 1. Handshake ping
            nlohmann::json ping_req;
            client_sock->read_json(ping_req);
            assert(ping_req["type"] == "ping");
            client_sock->send_json({{"type", "pong"}, {"model", "mock-model"}, {"n_ctx", 4096}, {"used_ctx", 50}});

            // 2. Chat request with command
            nlohmann::json chat_req;
            client_sock->read_json(chat_req);
            assert(chat_req["type"] == "chat");
            auto msgs = Protocol::parse_messages(chat_req["messages"]);
            assert(msgs.size() >= 2);
            assert(msgs.back().role == "user");
            assert(msgs.back().content == "echo 'mock command'");

            // Send streaming token and done
            client_sock->send_json({{"type", "token"}, {"piece", "Command executed successfully."}});
            client_sock->send_json({{"type", "done"}, {"response", "Command executed successfully."}, {"n_ctx", 4096}, {"used_ctx", 75}});
        });

        std::string conn_err;
        auto sock = TcpClient::connect(host, test_port, conn_err);
        assert(sock && sock->is_valid());

        // Ping handshake
        sock->send_json({{"type", "ping"}});
        nlohmann::json pong_resp;
        sock->read_json(pong_resp);
        assert(pong_resp["type"] == "pong");

        int n_ctx = pong_resp.value("n_ctx", 4096);
        int used_ctx = pong_resp.value("used_ctx", 0);

        std::string system_prompt = build_system_prompt(tools);
        std::vector<Protocol::ChatMessage> messages;
        messages.push_back({"system", system_prompt});

        // Simulate single command turn execution
        std::string command_input = "echo 'mock command'";
        messages.push_back({"user", command_input});

        nlohmann::json chat_req = {
            {"type", "chat"},
            {"messages", Protocol::messages_to_json(messages)},
            {"temperature", 0.7f},
            {"stream", true}
        };
        assert(sock->send_json(chat_req));

        std::string response;
        bool stream_ended = false;
        while (!stream_ended) {
            nlohmann::json stream_msg;
            assert(sock->read_json(stream_msg));
            std::string msg_type = stream_msg.value("type", "");
            if (msg_type == "token") {
                response += stream_msg.value("piece", "");
            } else if (msg_type == "done") {
                if (response.empty()) response = stream_msg.value("response", "");
                n_ctx = stream_msg.value("n_ctx", n_ctx);
                used_ctx = stream_msg.value("used_ctx", used_ctx);
                stream_ended = true;
            }
        }

        assert(response == "Command executed successfully.");
        assert(used_ctx == 75);

        server_thread.join();
    }

    std::cout << "[TEST] Command execution tests passed!" << std::endl;
}

void test_allowed_tools_and_quiet_mode() {
    std::cout << "[TEST] Running allowed tools and quiet mode tests..." << std::endl;

    // 1. Test parse_allowed_tools
    {
        auto t1 = parse_allowed_tools("web_fetch");
        assert(t1.size() == 1 && t1[0] == "web_fetch");

        auto t2 = parse_allowed_tools("web_fetch,read_file, write_file ");
        assert(t2.size() == 3);
        assert(t2[0] == "web_fetch");
        assert(t2[1] == "read_file");
        assert(t2[2] == "write_file");

        auto t3 = parse_allowed_tools("  WEB_FETCH ,  execute_command ,,, ");
        assert(t3.size() == 2);
        assert(t3[0] == "web_fetch");
        assert(t3[1] == "execute_command");

        auto t4 = parse_allowed_tools("");
        assert(t4.empty());
    }

    // 2. Test is_tool_allowed
    {
        // Auto-approve is true
        assert(is_tool_allowed("web_fetch", true, {}));
        assert(is_tool_allowed("execute_command", true, {}));

        // Auto-approve is false, no allowed tools
        assert(!is_tool_allowed("web_fetch", false, {}));

        // Auto-approve is false, specific allowed tools
        std::vector<std::string> allowed = {"web_fetch", "read_file"};
        assert(is_tool_allowed("web_fetch", false, allowed));
        assert(is_tool_allowed("WEB_FETCH", false, allowed));
        assert(is_tool_allowed("read_file", false, allowed));
        assert(!is_tool_allowed("write_file", false, allowed));
        assert(!is_tool_allowed("execute_command", false, allowed));

        // Wildcard
        std::vector<std::string> wildcard_allowed = {"*"};
        assert(is_tool_allowed("web_fetch", false, wildcard_allowed));
        assert(is_tool_allowed("execute_command", false, wildcard_allowed));
    }

    // 3. Test execution flow with allowed tool (e.g. web_fetch auto-approved)
    {
        std::vector<std::string> allowed_tools = {"web_fetch"};
        bool auto_approve = false;
        int execution_count = 0;

        auto execute_turn_mock = [&](const std::string & tool_name, const nlohmann::json & args, std::istream & in, std::ostream & out) -> std::string {
            if (!is_tool_allowed(tool_name, auto_approve, allowed_tools)) {
                ToolApproval approval = prompt_tool_approval(tool_name, args, in, out);
                if (approval == ToolApproval::ALWAYS) {
                    auto_approve = true;
                } else if (approval == ToolApproval::DENY) {
                    return "error: tool execution was denied by the user.";
                }
            }
            execution_count++;
            return "executed " + tool_name;
        };

        // web_fetch should execute without prompting
        std::istringstream empty_in("");
        std::ostringstream empty_out;
        std::string res1 = execute_turn_mock("web_fetch", {{"url", "https://example.com"}}, empty_in, empty_out);
        assert(res1 == "executed web_fetch");
        assert(execution_count == 1);
        assert(empty_out.str().empty()); // No prompt printed

        // other tool requires prompt
        std::istringstream in_no("nej\n");
        std::ostringstream prompt_out;
        std::string res2 = execute_turn_mock("execute_command", {{"command", "rm -rf /"}}, in_no, prompt_out);
        assert(res2.find("denied by the user") != std::string::npos);
        assert(execution_count == 1);
        assert(!prompt_out.str().empty()); // Prompt was printed
    }

    std::cout << "[TEST] Allowed tools and quiet mode tests passed!" << std::endl;
}

void test_strip_think_tags() {
    std::cout << "[TEST] Running strip think tags tests..." << std::endl;

    // 1. Standard think block
    {
        std::string input = "<think>Let me reason about this.</think>Final answer.";
        assert(strip_think_tags(input) == "Final answer.");
    }

    // 2. Multiline think block with whitespace
    {
        std::string input = "<think>\nStep 1: Calculate\nStep 2: Verify\n</think>\n\nResult is 42.";
        assert(strip_think_tags(input) == "Result is 42.");
    }

    // 3. Case insensitivity
    {
        std::string input = "<THINK>\nUppercase thinking block\n</THINK>\nHello world!";
        assert(strip_think_tags(input) == "Hello world!");

        std::string input2 = "<Think>Mixed case thinking</Think>42";
        assert(strip_think_tags(input2) == "42");
    }

    // 4. Orphaned closing tag from generation template prefix
    {
        std::string input = "Reasoning that started in prompt template...\n</think>\n\nActual final answer.";
        assert(strip_think_tags(input) == "Actual final answer.");
    }

    // 5. Multiple think blocks
    {
        std::string input = "<think>Block 1</think>Section 1\n<think>Block 2</think>\nSection 2";
        std::string res = strip_think_tags(input);
        assert(res.find("<think>") == std::string::npos);
        assert(res.find("</think>") == std::string::npos);
        assert(res.find("Section 1") != std::string::npos);
        assert(res.find("Section 2") != std::string::npos);
    }

    // 6. Unclosed think tag
    {
        std::string input = "<think>Unfinished thoughts that cut off";
        assert(strip_think_tags(input) == "");
    }

    // 7. Text without think tags
    {
        std::string input = "Just normal response text.";
        assert(strip_think_tags(input) == "Just normal response text.");
    }

    // 8. Think block only
    {
        std::string input = "<think>Only thoughts and nothing else</think>";
        assert(strip_think_tags(input) == "");

        std::string input_spaces = "<think>Thoughts</think>  \n\t  ";
        assert(strip_think_tags(input_spaces) == "");
    }

    // 9. Empty think tags & self-closing tags
    {
        assert(strip_think_tags("<think></think>42") == "42");
        assert(strip_think_tags("<think>\n</think>\n\n42") == "42");
        assert(strip_think_tags("<think>   </think>42") == "42");
        assert(strip_think_tags("<think/>42") == "42");
        assert(strip_think_tags("<think />42") == "42");
        assert(strip_think_tags("<thought></thought>42") == "42");
        assert(strip_think_tags("<thought/>42") == "42");
        assert(strip_think_tags("<reasoning></reasoning>42") == "42");
        assert(strip_think_tags("<reasoning/>42") == "42");
    }

    // 10. Alternative tags: <thought>, <reasoning>
    {
        assert(strip_think_tags("<thought>Thoughts here</thought>Answer") == "Answer");
        assert(strip_think_tags("<reasoning>Reasoning here</reasoning>Answer") == "Answer");
    }

    // 11. Stray tags and multiple blocks
    {
        std::string input = "Prefix reasoning </think> Actual content";
        assert(strip_think_tags(input) == "Actual content");
    }

    std::cout << "[TEST] Strip think tags tests passed!" << std::endl;
}

void test_thinking_stream_filter() {
    std::cout << "[TEST] Running ThinkingStreamFilter tests..." << std::endl;

    // 1. Empty think tags should be completely suppressed
    {
        std::string normal_out;
        std::string thinking_out;
        ThinkingStreamFilter filter([&](std::string_view piece, bool is_thinking) {
            if (is_thinking) thinking_out.append(piece);
            else normal_out.append(piece);
        });

        filter.process("<think></think>Hello world!");
        filter.flush();

        assert(thinking_out.empty());
        assert(normal_out == "Hello world!");
    }

    // 2. Whitespace-only think tags should be suppressed
    {
        std::string normal_out;
        std::string thinking_out;
        ThinkingStreamFilter filter([&](std::string_view piece, bool is_thinking) {
            if (is_thinking) thinking_out.append(piece);
            else normal_out.append(piece);
        });

        filter.process("<think>\n  \n</think>\nResult: 42");
        filter.flush();

        assert(thinking_out.empty());
        assert(normal_out.find("Result: 42") != std::string::npos);
        assert(normal_out.find("<think>") == std::string::npos);
        assert(normal_out.find("</think>") == std::string::npos);
    }

    // 3. Chunked empty think tags split across tokens
    {
        std::string normal_out;
        std::string thinking_out;
        ThinkingStreamFilter filter([&](std::string_view piece, bool is_thinking) {
            if (is_thinking) thinking_out.append(piece);
            else normal_out.append(piece);
        });

        filter.process("<thi");
        filter.process("nk>");
        filter.process("\n");
        filter.process("</th");
        filter.process("ink>");
        filter.process("Chunked test passed!");
        filter.flush();

        assert(thinking_out.empty());
        assert(normal_out == "Chunked test passed!");
    }

    // 4. Self-closing think tags
    {
        std::string normal_out;
        std::string thinking_out;
        ThinkingStreamFilter filter([&](std::string_view piece, bool is_thinking) {
            if (is_thinking) thinking_out.append(piece);
            else normal_out.append(piece);
        });

        filter.process("<think/>Direct response");
        filter.flush();

        assert(thinking_out.empty());
        assert(normal_out == "Direct response");
    }

    // 5. Short reasoning content
    {
        std::string normal_out;
        std::string thinking_out;
        ThinkingStreamFilter filter([&](std::string_view piece, bool is_thinking) {
            if (is_thinking) thinking_out.append(piece);
            else normal_out.append(piece);
        });

        filter.process("<think>Quick thought</think>Final answer");
        filter.flush();

        assert(thinking_out.find("Quick thought") != std::string::npos);
        assert(normal_out == "Final answer");
    }

    // 6. Long streamed reasoning
    {
        std::string normal_out;
        std::string thinking_out;
        ThinkingStreamFilter filter([&](std::string_view piece, bool is_thinking) {
            if (is_thinking) thinking_out.append(piece);
            else normal_out.append(piece);
        });

        filter.process("<think>");
        filter.process("This is a very detailed reasoning chain that exceeds forty characters in length so it enters streaming mode.");
        filter.process("</think>");
        filter.process("Final output after reasoning.");
        filter.flush();

        assert(thinking_out.find("detailed reasoning") != std::string::npos);
        assert(normal_out == "Final output after reasoning.");
    }

    // 7. Stray closing tag
    {
        std::string normal_out;
        std::string thinking_out;
        ThinkingStreamFilter filter([&](std::string_view piece, bool is_thinking) {
            if (is_thinking) thinking_out.append(piece);
            else normal_out.append(piece);
        });

        filter.process("</think>Direct answer after prompt prefill");
        filter.flush();

        assert(thinking_out.empty());
        assert(normal_out.find("Direct answer") != std::string::npos);
        assert(normal_out.find("</think>") == std::string::npos);
    }

    std::cout << "[TEST] ThinkingStreamFilter tests passed!" << std::endl;
}

void test_agent_backend_and_common_runner() {
    std::cout << "[TEST] Running agent backend and common runner tests..." << std::endl;

    // 1. Mock Agent Backend
    class MockAgentBackend : public IAgentBackend {
    public:
        int chat_call_count = 0;
        int generate_call_count = 0;
        int reset_call_count = 0;
        int n_ctx = 4096;
        int used_ctx = 100;
        std::vector<std::string> chat_responses;
        std::string generate_response = "Generated summary of conversation.";

        std::string chat(std::span<const Protocol::ChatMessage> messages,
                         AgentTokenCallback token_cb = nullptr,
                         float temp_override = -1.0f) override {
            (void)messages;
            (void)temp_override;
            std::string resp = chat_responses.empty() ? "Default response" : chat_responses[chat_call_count % chat_responses.size()];
            chat_call_count++;
            if (token_cb) {
                token_cb(resp);
            }
            return resp;
        }

        std::string generate(std::string_view prompt,
                             AgentTokenCallback token_cb = nullptr,
                             float temp_override = -1.0f) override {
            (void)prompt;
            (void)temp_override;
            generate_call_count++;
            if (token_cb) {
                token_cb(generate_response);
            }
            return generate_response;
        }

        int get_context_size() override { return n_ctx; }
        int get_used_context() override { return used_ctx; }
        void reset_context() override {
            reset_call_count++;
            used_ctx = 0;
        }
        std::string get_model_name() const override { return "mock_model.gguf"; }
    };

    // 2. Test compact_context with MockAgentBackend
    {
        MockAgentBackend mock;
        std::vector<Protocol::ChatMessage> msgs = {
            {"system", "System instruction"},
            {"user", "Message 1"},
            {"assistant", "Response 1"},
            {"user", "Message 2"},
            {"assistant", "Response 2"}
        };
        bool ok = compact_context(mock, msgs, true);
        assert(ok);
        assert(mock.generate_call_count == 1);
        assert(mock.reset_call_count == 1);
        assert(msgs.size() == 5); // system + user summary + assistant ack + keep_recent(2)
        assert(msgs[1].content.find("Generated summary of conversation.") != std::string::npos);
    }

    // 3. Test execute_agent_turn with direct tool execution
    {
        MockAgentBackend mock;
        auto tools = get_registered_tools(false);
        std::vector<Protocol::ChatMessage> messages;
        messages.push_back({"system", "System instruction"});

        bool auto_approve = true;
        std::string out_resp;
        // Test slash command execution
        bool turn_ok = execute_agent_turn(mock, messages, tools, "/exec echo 'test_common_runner'",
                                          0.7f, 10, auto_approve, {}, &out_resp, true);
        assert(turn_ok);
        assert(out_resp.find("test_common_runner") != std::string::npos);

        // Test normal agent turn with tool call then final answer
        mock.chat_responses = {
            "<tool_call>{\"name\":\"file_search\",\"arguments\":{\"path\":\"/tmp\",\"pattern\":\"xyz_non_existent\"}}</tool_call>",
            "I found no files matching pattern."
        };
        turn_ok = execute_agent_turn(mock, messages, tools, "Find xyz files in /tmp",
                                     0.7f, 10, auto_approve, {}, &out_resp, true);
        assert(turn_ok);
        assert(mock.chat_call_count == 2);
        assert(out_resp == "I found no files matching pattern.");
        assert(messages.size() >= 4); // sys, user, assistant(tool_call), tool(response), assistant(final)

        // Test agent turn with multiple tool calls in a single turn response (controlled sequential execution)
        mock.chat_call_count = 0;
        messages.clear();
        messages.push_back({"system", "System instruction"});
        mock.chat_responses = {
            "I will run two commands:\n"
            "<tool_call>{\"name\":\"execute_command\",\"arguments\":{\"command\":\"echo multi_1\"}}</tool_call>\n"
            "<tool_call>{\"name\":\"execute_command\",\"arguments\":{\"command\":\"echo multi_2\"}}</tool_call>",
            "Both commands executed successfully."
        };
        turn_ok = execute_agent_turn(mock, messages, tools, "Run both commands",
                                     0.7f, 10, auto_approve, {}, &out_resp, true);
        assert(turn_ok);
        assert(mock.chat_call_count == 2);
        assert(out_resp == "Both commands executed successfully.");
        // Check message sequence: system, user, assistant(with both tool calls), tool1, tool2, assistant(final)
        assert(messages.size() == 6);
        assert(messages[0].role == "system");
        assert(messages[1].role == "user");
        assert(messages[2].role == "assistant");
        assert(messages[3].role == "tool");
        assert(messages[3].content.find("multi_1") != std::string::npos);
        assert(messages[4].role == "tool");
        assert(messages[4].content.find("multi_2") != std::string::npos);
        assert(messages[5].role == "assistant");
        assert(messages[5].content == "Both commands executed successfully.");

        // Test agent turn recovering from broken tool_call JSON
        mock.chat_call_count = 0;
        messages.clear();
        messages.push_back({"system", "System instruction"});
        mock.chat_responses = {
            // First turn: model outputs invalid JSON inside <tool_call>
            "<tool_call>\n{\n  \"name\": \"execute_command\",\n  \"arguments\": {\n    \"command\": \n  }\n}\n</tool_call>",
            // Second turn: model sees nlohmann error in <tool_response> and corrects it
            "<tool_call>{\"name\":\"execute_command\",\"arguments\":{\"command\":\"echo fixed_syntax\"}}</tool_call>",
            // Third turn: final answer
            "Command executed with syntax correction."
        };
        turn_ok = execute_agent_turn(mock, messages, tools, "Run echo command",
                                     0.7f, 10, auto_approve, {}, &out_resp, true);
        assert(turn_ok);
        assert(mock.chat_call_count == 3);
        assert(out_resp == "Command executed with syntax correction.");
        assert(messages.size() == 7);
        assert(messages[0].role == "system");
        assert(messages[1].role == "user");
        assert(messages[2].role == "assistant"); // broken tool call
        assert(messages[3].role == "tool");      // nlohmann parse error response
        assert(messages[3].content.find("<tool_response>") != std::string::npos);
        assert(messages[3].content.find("error:") != std::string::npos);
        assert(messages[3].content.find("parse_error") != std::string::npos || messages[3].content.find("[json.exception") != std::string::npos || messages[3].content.find("syntax error") != std::string::npos);
        assert(messages[4].role == "assistant"); // fixed tool call
        assert(messages[5].role == "tool");      // tool output
        assert(messages[5].content.find("fixed_syntax") != std::string::npos);
        assert(messages[6].role == "assistant"); // final answer

        // Test agent turn where response contains <tool_call> in the middle of text (not at end)
        mock.chat_call_count = 0;
        messages.clear();
        messages.push_back({"system", "System instruction"});
        mock.chat_responses = {
            "You can execute commands with <tool_call>{\"name\":\"execute_command\",\"arguments\":{\"command\":\"ls\"}}</tool_call> in your prompts. Let me know if you have questions!"
        };
        turn_ok = execute_agent_turn(mock, messages, tools, "Explain how tool calling works",
                                     0.7f, 10, auto_approve, {}, &out_resp, true);
        assert(turn_ok);
        assert(mock.chat_call_count == 1);
        assert(out_resp == "You can execute commands with <tool_call>{\"name\":\"execute_command\",\"arguments\":{\"command\":\"ls\"}}</tool_call> in your prompts. Let me know if you have questions!");
        assert(messages.size() == 3); // system, user, assistant (no tool execution)
        assert(messages[2].role == "assistant");
    }

    // 4. Test run_subagent with MockAgentBackend
    {
        MockAgentBackend mock;
        mock.chat_responses = {
            "Sub-agent final answer directly provided."
        };
        auto base_tools = get_base_tools();
        bool auto_approve = true;
        std::string sub_ans = run_subagent(mock, "Delegated sub-task", base_tools, "",
                                           0.7f, 10, auto_approve, {}, true);
        assert(sub_ans == "Sub-agent final answer directly provided.");
    }

    // 5. Test CLI parser helpers
    {
        AgentSessionConfig cfg;
        const char * argv[] = {
            "agent",
            "-t", "0.5",
            "-it", "15",
            "--command", "echo 123",
            "-q",
            "--allow-tool", "web_fetch",
            "--allow-tools", "read_file,write_file",
            "--sub-agents",
            "-y",
            "-s", "Custom prompt"
        };
        int argc = sizeof(argv) / sizeof(argv[0]);
        for (int i = 1; i < argc; i++) {
            assert(parse_agent_cli_arg(i, argc, const_cast<char**>(argv), cfg, true));
        }
        assert(cfg.temperature == 0.5f);
        assert(cfg.max_iterations == 15);
        assert(cfg.single_command == "echo 123");
        assert(cfg.quiet == true);
        assert(cfg.allowed_tools.size() == 3);
        assert(cfg.enable_subagents == true);
        assert(cfg.auto_approve == true);
        assert(cfg.custom_system_prompt == "Custom prompt");

        // Test disabling sub-agents via CLI
        const char * argv_no_sa[] = {"agent", "--no-sub-agents"};
        int argc_no_sa = sizeof(argv_no_sa) / sizeof(argv_no_sa[0]);
        for (int i = 1; i < argc_no_sa; i++) {
            assert(parse_agent_cli_arg(i, argc_no_sa, const_cast<char**>(argv_no_sa), cfg, true));
        }
        assert(cfg.enable_subagents == false);
    }

    std::cout << "[TEST] Agent backend and common runner tests passed!" << std::endl;
}

void test_components() {
    test_tools();
    test_search_text();
    test_tool_call_parsing();
    test_protocol_and_sockets();
    test_utf8_streaming();
    test_utf8_json_resilience();
    test_context_management();
    test_context_compaction();
    test_subagents();
    test_tool_approval();
    test_ai_instructions();
    test_command_execution();
    test_allowed_tools_and_quiet_mode();
    test_strip_think_tags();
    test_thinking_stream_filter();
    test_agent_backend_and_common_runner();
    std::cout << "\nALL TESTS PASSED SUCCESSFULLY!" << std::endl;
}
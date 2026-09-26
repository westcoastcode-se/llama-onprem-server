#include "cli/agent.hpp"
#include "common/std.hpp"
#include "api/errors.hpp"
#include "api/models.hpp"
#include "api/sessions.hpp"
#include "client/rest_client.hpp"
#include "common/log.hpp"

#include <CLI11.hpp>

#include <cctype>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <print>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{

struct Options
{
    std::string host = "127.0.0.1";
    int port = 8080;
    bool json = false;
    bool verbose = false;
};

RestClient connect(const Options &options)
{
    Logger::set_level(options.verbose ? Logger::LEVEL_INFO : Logger::LEVEL_ERROR);
    return RestClient(options.host, options.port);
}

SessionID parse_session_id(std::string_view text)
{
    if (text.empty() || text.find_first_not_of("0123456789") != std::string_view::npos)
    {
        throw std::runtime_error("session id must be a positive integer");
    }
    std::size_t consumed = 0;
    const unsigned long long value = std::stoull(std::string(text), &consumed, 10);
    if (consumed != text.size() || value == 0)
    {
        throw std::runtime_error("session id must be a positive integer");
    }
    return static_cast<SessionID>(value);
}

std::string read_stdin()
{
    std::ostringstream buffer;
    buffer << std::cin.rdbuf();
    std::string text = buffer.str();
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
    {
        text.pop_back();
    }
    return text;
}

std::string join_words(const std::vector<std::string> &words)
{
    std::string text;
    for (const auto &word : words)
    {
        if (!text.empty())
        {
            text.push_back(' ');
        }
        text += word;
    }
    return text;
}

void print_session(const SessionResponse &session, bool as_json)
{
    if (as_json)
    {
        std::println("{}", session.to_json().dump(2));
        return;
    }

    std::println("session {}", session.id);
    std::println("state {}", session.state.to_string());
    std::println("questions {}", session.questions ? "true" : "false");
    if (session.active_job_key)
    {
        std::println("active_job {}", *session.active_job_key);
    }
    if (!session.error.empty())
    {
        std::println("error {}", session.error);
    }
    if (!session.error_code.empty())
    {
        std::println("error_code {}", session.error_code);
    }
    for (const ChatMessage &message : session.messages)
    {
        std::println("--- {}", message.role);
        if (!message.reasoning_content.empty())
        {
            std::println("thinking:");
            std::println("{}", message.reasoning_content);
        }
        if (!message.content.empty())
        {
            std::println("{}", message.content);
        }
    }
    for (const ParsedToolCall &call : session.pending_tool_calls)
    {
        std::println("tool {} {} {}", call.id, call.name, call.arguments.dump());
    }
    if (session.pending_question)
    {
        std::println("question {}", session.pending_question->text);
        for (std::size_t i = 0; i < session.pending_question->answers.size(); ++i)
        {
            std::println("  {} {}", i, session.pending_question->answers[i]);
        }
    }
}

void print_pause(const SessionResponse &session)
{
    if (session.state.value == SessionState::AwaitingTools)
    {
        std::println(stderr, "awaiting tools");
        for (const ParsedToolCall &call : session.pending_tool_calls)
        {
            std::println(stderr, "  {} {} {}", call.id, call.name, call.arguments.dump());
        }
        std::println(stderr, "submit results with: callisto_cli tools {} ...", session.id);
    }
    else if (session.state.value == SessionState::AwaitingQuestion && session.pending_question)
    {
        std::println(stderr, "question: {}", session.pending_question->text);
        for (std::size_t i = 0; i < session.pending_question->answers.size(); ++i)
        {
            std::println(stderr, "  {} {}", i, session.pending_question->answers[i]);
        }
    }
}

void stream_turn(RestClient &client, SessionID session_id, JobKey key)
{
    try
    {
        client.stream_tokens(session_id, key, [](const string &piece) {
            std::print("{}", piece);
            return true;
        });
        std::println("");
    }
    catch (...)
    {
        std::println("");
        throw;
    }
    print_pause(client.get_session(session_id));
}

ChatMessage parse_history_item(std::string_view text)
{
    ChatMessage message;
    const auto colon = text.find(':');
    if (colon == std::string_view::npos || colon == 0)
    {
        message.role = "user";
        message.content = std::string(text);
    }
    else
    {
        message.role = std::string(text.substr(0, colon));
        message.content = std::string(text.substr(colon + 1));
    }
    message.validate();
    return message;
}

json load_tool_results(const std::string &path, const std::string &id, const std::string &name,
                       const std::string &content, bool denied, bool from_flags)
{
    if (!path.empty())
    {
        std::ifstream in(path);
        if (!in)
        {
            throw std::runtime_error("could not read " + path);
        }
        std::ostringstream buffer;
        buffer << in.rdbuf();
        const json parsed = json::parse(buffer.str());
        if (parsed.is_array())
        {
            return parsed;
        }
        if (parsed.is_object() && parsed.contains("tool_results") && parsed["tool_results"].is_array())
        {
            return parsed["tool_results"];
        }
        if (parsed.is_object() && parsed.contains("results") && parsed["results"].is_array())
        {
            return parsed["results"];
        }
        throw std::runtime_error("tool file must be an array or an object with tool_results");
    }
    if (!from_flags)
    {
        throw std::runtime_error("pass --file or --name and --content");
    }
    return json::array({json{{"id", id}, {"name", name}, {"content", content}, {"denied", denied}}});
}

void add_connection(CLI::App &app, Options &options)
{
    app.add_option("--host", options.host, "Server host")->default_val("127.0.0.1");
    app.add_option("-p,--port", options.port, "Server port")->default_val(8080)->check(CLI::Range(1, 65535));
    app.add_flag("--json", options.json, "Print the API object as JSON");
    app.add_flag("-v,--verbose", options.verbose, "Log HTTP client details");
    app.fallthrough();
}

} // namespace

int main(int argc, char **argv)
{
    CLI::App app{"Coding agent for the local Callisto server"};
    app.set_help_all_flag("--help-all", "Show help for every subcommand");
    app.footer("With no subcommand, start an interactive session in the current directory.\n"
               "callisto_client is the older line client. health, session, send, job, and tools talk to the HTTP API.");

    Options options;
    add_connection(app, options);
    std::string approval = "read-only";
    bool resume = false;
    bool show_think = true;
    bool questions = true;
    std::string resume_id;
    std::vector<std::string> initial_prompt;
    app.add_option("--approval", approval, "read-only, auto, or full")->default_val("read-only");
    app.add_flag("--resume", resume, "Continue the saved session for this directory and server");
    app.add_option("--session", resume_id, "Continue this session id");
    app.add_flag("--show-think,!--hide-think", show_think, "Print the thinking block");
    app.add_flag("--questions,!--no-questions", questions, "Let the model pause and ask a question");
    app.add_option("prompt", initial_prompt, "Task to start with")->expected(0, -1);

    int agent_status = 0;
    auto start_agent = [&](bool exec_mode, const std::string &prompt) {
        if (approval != "read-only" && approval != "suggest" && approval != "auto" && approval != "full")
        {
            throw std::runtime_error("--approval must be read-only, auto, or full");
        }
        AgentConfig config;
        config.host = options.host;
        config.port = options.port;
        config.show_think = show_think;
        config.questions = questions;
        config.exec = exec_mode;
        config.resume = resume;
        config.prompt = prompt;
        if (approval == "auto")
        {
            config.approval = ApprovalMode::Auto;
        }
        else if (approval == "full")
        {
            config.approval = ApprovalMode::Full;
        }
        else
        {
            config.approval = ApprovalMode::ReadOnly;
        }
        if (!resume_id.empty())
        {
            config.session = parse_session_id(resume_id);
        }
        agent_status = run_agent(config);
    };

    auto *exec_task = app.add_subcommand("exec", "Run one task and exit");
    exec_task->fallthrough();
    std::vector<std::string> exec_words;
    exec_task->add_option("prompt", exec_words, "Task")->required()->expected(-1);
    exec_task->callback([&] { start_agent(true, join_words(exec_words)); });

    auto *health = app.add_subcommand("health", "GET /health");
    health->fallthrough();
    health->callback([&] {
        RestClient client = connect(options);
        if (!client.health())
        {
            throw std::runtime_error(std::format("server not reachable at {}", client.base_url()));
        }
        std::println("OK");
    });

    auto *session = app.add_subcommand("session", "Create, read, clone, and delete sessions");
    session->require_subcommand(1);
    session->fallthrough();

    auto *create = session->add_subcommand("create", "POST /v1/sessions");
    create->fallthrough();
    std::string system_text;
    bool create_questions = true;
    int max_tokens = -1;
    std::vector<std::string> history;
    std::vector<std::string> tool_json;
    create->add_option("-s,--system", system_text, "Extra system instructions");
    create->add_flag("--questions,!--no-questions", create_questions, "Pause when the model asks a question");
    create->add_option("--max-tokens", max_tokens, "Turn cap. Negative means no session cap")->default_val(-1);
    create->add_option("-m,--message", history, "History entry as role:text")->take_all();
    create->add_option("--tool", tool_json, "Tool JSON, OpenAI function or {name,description,parameters}")->take_all();
    create->callback([&] {
        CreateSessionRequest request;
        request.system = system_text;
        request.questions = create_questions;
        request.max_tokens = max_tokens;
        for (const auto &item : history)
        {
            request.messages.push_back(parse_history_item(item));
        }
        for (const auto &item : tool_json)
        {
            request.tools.push_back(ChatTool::from_json(json::parse(item)));
        }
        print_session(connect(options).create_session(request), options.json);
    });

    auto *get = session->add_subcommand("get", "GET /v1/sessions/{id}");
    get->fallthrough();
    std::string get_id;
    get->add_option("id", get_id, "Session id")->required();
    get->callback([&] { print_session(connect(options).get_session(parse_session_id(get_id)), options.json); });

    auto *remove = session->add_subcommand("delete", "DELETE /v1/sessions/{id}");
    remove->alias("rm");
    remove->fallthrough();
    std::string delete_id;
    remove->add_option("id", delete_id, "Session id")->required();
    remove->callback([&] {
        const SessionID id = parse_session_id(delete_id);
        connect(options).delete_session(id);
        if (options.json)
        {
            std::println("{}", json{{"deleted", true}}.dump(2));
        }
        else
        {
            std::println("deleted {}", id);
        }
    });

    auto *snapshot = session->add_subcommand("snapshot", "POST /v1/sessions/{id}/snapshots");
    snapshot->fallthrough();
    std::string snapshot_id;
    snapshot->add_option("id", snapshot_id, "Parent session id")->required();
    snapshot->callback(
        [&] { print_session(connect(options).snapshot_session(parse_session_id(snapshot_id)), options.json); });

    auto *send = app.add_subcommand("send", "POST /v1/sessions/{id}/messages and stream the reply");
    send->fallthrough();
    std::string send_id;
    std::string send_role = "user";
    int send_max_tokens = -1;
    std::vector<std::string> send_words;
    send->add_option("id", send_id, "Session id")->required();
    send->add_option("text", send_words, "Message text. Omit to read stdin")->expected(0, -1);
    send->add_option("--role", send_role, "Message role")->default_val("user");
    send->add_option("--max-tokens", send_max_tokens, "Cap for this turn")->default_val(-1);
    send->callback([&] {
        std::string text = join_words(send_words);
        if (text.empty() || text == "-")
        {
            text = read_stdin();
        }
        SessionMessageRequest message;
        message.content = std::move(text);
        message.role = send_role;
        message.max_tokens = send_max_tokens;
        RestClient client = connect(options);
        const SessionID id = parse_session_id(send_id);
        const auto queued = client.post_message(id, message);
        stream_turn(client, id, queued.key);
    });

    auto *job = app.add_subcommand("job", "Read, stream, or cancel a generation");
    job->require_subcommand(1);
    job->fallthrough();

    auto *job_get = job->add_subcommand("get", "GET /v1/sessions/{id}/jobs/{job}");
    job_get->fallthrough();
    std::string job_get_session;
    std::string job_get_key;
    job_get->add_option("id", job_get_session, "Session id")->required();
    job_get->add_option("job", job_get_key, "Job key")->required();
    job_get->callback([&] {
        const json status =
            connect(options).get_job(parse_session_id(job_get_session), parse_session_id(job_get_key));
        if (options.json)
        {
            std::println("{}", status.dump(2));
            return;
        }
        std::println("job {}", status.value("key", 0));
        std::println("state {}", status.value("state", ""));
        if (status.contains("error") && !status["error"].get<std::string>().empty())
        {
            std::println("error {}", status["error"].get<std::string>());
        }
        if (status.contains("error_code"))
        {
            std::println("error_code {}", status["error_code"].get<std::string>());
        }
        if (status.contains("reasoning_content"))
        {
            std::println("thinking:");
            std::println("{}", status["reasoning_content"].get<std::string>());
        }
        const std::string content = status.value("content", "");
        if (!content.empty())
        {
            std::println("{}", content);
        }
    });

    auto *job_stream = job->add_subcommand("stream", "GET /v1/sessions/{id}/jobs/{job}/tokens");
    job_stream->fallthrough();
    std::string job_stream_session;
    std::string job_stream_key;
    job_stream->add_option("id", job_stream_session, "Session id")->required();
    job_stream->add_option("job", job_stream_key, "Job key")->required();
    job_stream->callback([&] {
        RestClient client = connect(options);
        stream_turn(client, parse_session_id(job_stream_session), parse_session_id(job_stream_key));
    });

    auto *job_cancel = job->add_subcommand("cancel", "DELETE /v1/sessions/{id}/jobs/{job}");
    job_cancel->fallthrough();
    std::string job_cancel_session;
    std::string job_cancel_key;
    job_cancel->add_option("id", job_cancel_session, "Session id")->required();
    job_cancel->add_option("job", job_cancel_key, "Job key")->required();
    job_cancel->callback([&] {
        RestClient client = connect(options);
        const SessionID id = parse_session_id(job_cancel_session);
        const JobKey key = parse_session_id(job_cancel_key);
        if (!client.cancel_job(id, key))
        {
            throw std::runtime_error(std::format("could not cancel job {} on session {}", key, id));
        }
        if (options.json)
        {
            std::println("{}", json{{"cancelled", true}}.dump(2));
        }
        else
        {
            std::println("cancelled {}", key);
        }
    });

    auto *tools = app.add_subcommand("tools", "POST /v1/sessions/{id}/tools and stream the reply");
    tools->fallthrough();
    std::string tools_session;
    std::string tools_file;
    std::string tools_id = "1";
    std::string tools_name;
    std::string tools_content;
    bool tools_denied = false;
    bool tools_from_flags = false;
    tools->add_option("id", tools_session, "Session id")->required();
    tools->add_option("--file", tools_file, "JSON array, or an object with tool_results");
    tools->add_option("--call-id", tools_id, "Tool call id")->default_val("1");
    tools->add_option("--name", tools_name, "Tool name");
    tools->add_option("--content", tools_content, "Tool output");
    tools->add_flag("--denied", tools_denied, "The user denied this call");
    tools->callback([&] {
        tools_from_flags = !tools_name.empty() || !tools_content.empty() || tools_denied;
        const json results =
            load_tool_results(tools_file, tools_id, tools_name, tools_content, tools_denied, tools_from_flags);
        RestClient client = connect(options);
        const SessionID id = parse_session_id(tools_session);
        const json queued = client.post_tool_results(id, results);
        stream_turn(client, id, queued.at("key").get<JobKey>());
    });

    app.callback([&] {
        if (!app.get_subcommands().empty())
        {
            return;
        }
        start_agent(false, join_words(initial_prompt));
    });

    try
    {
        app.parse(argc, argv);
    }
    catch (const CLI::ParseError &error)
    {
        return app.exit(error);
    }
    catch (const RestClient::ClientError &error)
    {
        std::println(stderr, "{}", error.what());
        if (error.code == kContextFull)
        {
            std::println(stderr, "the message is not in the session. Compact the history and send it again.");
            return 2;
        }
        return 1;
    }
    catch (const std::exception &error)
    {
        std::println(stderr, "{}", error.what());
        return 1;
    }
    return agent_status;
}

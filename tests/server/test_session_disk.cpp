#include "common/defer.hpp"
#include "server/agent/model_adapter.hpp"
#include "server/sessions/session_disk.hpp"
#include "server/sessions/sessions.hpp"
#include "../tests.hpp"

#include <nlohmann/json.hpp>

namespace
{

ChatMessage message(std::string role, std::string content, std::string reasoning = {})
{
    return ChatMessage{.role = std::move(role),
                       .content = std::move(content),
                       .reasoning_content = std::move(reasoning),
                       .tool_call_id = {},
                       .tool_name = {},
                       .tool_calls = {}};
}

SessionRecord awaiting_tools_record()
{
    SessionRecord record;
    record.id = 42;
    record.system_prompt = "be brief";
    record.messages.push_back(message("user", "hello"));
    record.messages.push_back(message("assistant", "calling", "think"));
    record.tools.push_back(ChatTool{.name = "read_file", .description = "read", .parameters = R"({"type":"object"})"});
    record.questions = false;
    record.compress_tools = false;
    record.max_tokens = 128;
    record.state = SessionState::AwaitingTools;
    ParsedToolCall call;
    call.id = "c1";
    call.name = "read_file";
    call.arguments = nlohmann::json{{"path", "a.cpp"}};
    record.pending_tool_calls.push_back(std::move(call));
    record.pending_question = ParsedQuestion{.text = "which?", .answers = {"a", "b"}};
    record.error = "earlier";
    record.error_code = "context_full";
    record.context_used = 10;
    record.context_size = 4096;
    record.updated_at = 1'700'000'000;
    return record;
}

} // namespace

/**
 * A session file round-trips the conversation. Junk, a temp write, and an id that
 * does not match its filename are left out, and remove deletes the record.
 */
static int test_session_record_round_trip()
{
    const auto dir = make_temp_dir("session-disk");
    defer(std::filesystem::remove_all(dir));

    SessionRecord empty_id;
    assertTrue(!write_session_record({}, awaiting_tools_record()));
    assertTrue(!write_session_record(dir, empty_id));
    assertTrue(read_session_records(dir / "missing").empty());

    assertTrue(write_session_record(dir, awaiting_tools_record()));
    SessionRecord older;
    older.id = 7;
    older.state = SessionState::Idle;
    older.system_prompt = "older";
    assertTrue(write_session_record(dir, older));

    write_test_file(dir / "11.json", R"({"id":99,"state":"idle"})");
    write_test_file(dir / "12.json.tmp", R"({"id":12,"state":"idle"})");
    write_test_file(dir / "notes.json", R"({"id":1,"state":"idle"})");
    write_test_file(dir / "0.json", R"({"id":0,"state":"idle"})");
    write_test_file(dir / "13.json", "{");
    write_test_file(dir / "14.json", R"({"id":14,"state":"nope"})");

    const auto records = read_session_records(dir);
    assertEquals(static_cast<size_t>(2), records.size());
    assertEquals(static_cast<SessionID>(42), records[0].id);
    assertEquals(static_cast<SessionID>(7), records[1].id);
    assertEquals(std::string("be brief"), records[0].system_prompt);
    assertEquals(static_cast<size_t>(2), records[0].messages.size());
    assertEquals(std::string("think"), records[0].messages[1].reasoning_content);
    assertEquals(static_cast<size_t>(1), records[0].tools.size());
    assertEquals(std::string("read_file"), records[0].tools[0].name);
    assertTrue(records[0].state == SessionState::AwaitingTools);
    assertEquals(static_cast<size_t>(1), records[0].pending_tool_calls.size());
    assertEquals(std::string("a.cpp"), records[0].pending_tool_calls[0].arguments.at("path").get<std::string>());
    assertTrue(records[0].pending_question.has_value());
    assertEquals(std::string("which?"), records[0].pending_question->text);
    assertEquals(static_cast<size_t>(2), records[0].pending_question->answers.size());
    assertTrue(!records[0].questions);
    assertTrue(!records[0].compress_tools);
    assertEquals(128, records[0].max_tokens);
    assertEquals(std::string("earlier"), records[0].error);
    assertEquals(std::string("context_full"), records[0].error_code);
    assertEquals(10, records[0].context_used);
    assertEquals(4096, records[0].context_size);
    assertEquals(static_cast<int64_t>(1'700'000'000), records[0].updated_at);

    write_test_file(dir / "42.json.tmp", "partial");
    remove_session_record(dir, 42);
    assertTrue(!std::filesystem::exists(dir / "42.json"));
    assertTrue(!std::filesystem::exists(dir / "42.json.tmp"));
    const auto left = read_session_records(dir);
    assertEquals(static_cast<size_t>(1), left.size());
    assertEquals(static_cast<SessionID>(7), left[0].id);
    return EXIT_SUCCESS;
}

/**
 * Restoring keeps a finished wait. A generation that died with the process becomes idle
 * and does not keep the id of a new session.
 */
static int test_session_restore_keeps_id_and_drops_generation()
{
    QwenAdapter adapter;
    Session session(adapter, 42);
    session.restore(awaiting_tools_record());

    assertEquals(static_cast<SessionID>(42), session.id);
    const SessionResponse waiting = session.to_response(true);
    assertTrue(waiting.state == SessionState::AwaitingTools);
    assertEquals(std::string("be brief"), waiting.system_prompt);
    assertEquals(static_cast<size_t>(2), waiting.messages.size());
    assertEquals(std::string("think"), waiting.messages[1].reasoning_content);
    assertEquals(static_cast<size_t>(1), waiting.pending_tool_calls.size());
    assertEquals(std::string("read_file"), waiting.pending_tool_calls[0].name);
    assertTrue(waiting.pending_question.has_value());
    assertEquals(std::string("which?"), waiting.pending_question->text);
    assertTrue(!waiting.questions);
    assertTrue(!waiting.compress_tools);
    assertEquals(std::string("earlier"), waiting.error);
    assertEquals(std::string("context_full"), waiting.error_code);
    assertEquals(10, waiting.context_used);
    assertEquals(4096, waiting.context_size);
    assertTrue(!session.active_job().has_value());
    assertEquals(128, session.record().max_tokens);
    assertEquals(static_cast<int64_t>(1'700'000'000), session.updated_at());
    assertTrue(!session.is_gc_idle());
    session.mark_updated();
    assertTrue(session.updated_at() > 1'700'000'000);
    assertTrue(session.record().updated_at > 1'700'000'000);

    SessionRecord generating;
    generating.id = 99;
    generating.state = SessionState::Generating;
    generating.messages.push_back(message("user", "continue"));
    ParsedToolCall call;
    call.id = "c2";
    call.name = "read_file";
    call.arguments = nlohmann::json::object();
    generating.pending_tool_calls.push_back(std::move(call));
    generating.pending_question = ParsedQuestion{.text = "later?", .answers = {}};

    Session resumed(adapter, 42);
    resumed.restore(std::move(generating));
    assertEquals(static_cast<SessionID>(42), resumed.id);
    const SessionResponse idle = resumed.to_response(true);
    assertTrue(idle.state == SessionState::Idle);
    assertTrue(idle.pending_tool_calls.empty());
    assertTrue(!idle.pending_question.has_value());
    assertEquals(static_cast<size_t>(1), idle.messages.size());
    assertEquals(std::string("continue"), idle.messages[0].content);
    assertTrue(!resumed.active_job().has_value());
    assertTrue(resumed.is_gc_idle());
    return EXIT_SUCCESS;
}

/**
 * The size cap drops the oldest unpinned session first and keeps one the caller just resumed.
 * A kv file with no conversation is removed only after those sessions.
 */
static int test_session_cache_drops_oldest()
{
    const auto dir = make_temp_dir("session-cache");
    defer(std::filesystem::remove_all(dir));

    write_test_file(dir / "1.json", std::string(10, 'a'));
    write_test_file(dir / "1.kv", std::string(100, 'b'));
    write_test_file(dir / "2.json", std::string(10, 'a'));
    write_test_file(dir / "2.kv", std::string(100, 'b'));
    write_test_file(dir / "3.json", std::string(10, 'a'));
    write_test_file(dir / "3.kv", std::string(100, 'b'));
    write_test_file(dir / "9.kv", std::string(50, 'c'));

    const SessionCacheRef loaded[] = {
        SessionCacheRef{.id = 1, .updated_at = 1, .pinned = true},
        SessionCacheRef{.id = 2, .updated_at = 2, .pinned = false},
        SessionCacheRef{.id = 3, .updated_at = 3, .pinned = false},
    };
    const SessionCachePlan open = plan_session_cache(dir, loaded, 0);
    assertTrue(open.drop_sessions.empty());
    assertTrue(open.drop_orphan_kv.empty());
    assertTrue(!open.still_over);

    const SessionCachePlan fit = plan_session_cache(dir, loaded, 250);
    assertEquals(static_cast<size_t>(2), fit.drop_sessions.size());
    assertEquals(static_cast<SessionID>(2), fit.drop_sessions[0]);
    assertEquals(static_cast<SessionID>(3), fit.drop_sessions[1]);
    assertTrue(fit.drop_orphan_kv.empty());
    assertTrue(!fit.still_over);

    const SessionCachePlan tight = plan_session_cache(dir, loaded, 100);
    assertEquals(static_cast<size_t>(2), tight.drop_sessions.size());
    assertEquals(static_cast<SessionID>(2), tight.drop_sessions[0]);
    assertEquals(static_cast<SessionID>(3), tight.drop_sessions[1]);
    assertEquals(static_cast<size_t>(1), tight.drop_orphan_kv.size());
    assertEquals(static_cast<SessionID>(9), tight.drop_orphan_kv[0]);
    assertTrue(tight.still_over);
    return EXIT_SUCCESS;
}

/**
 * A separate KV directory is counted on its own. A .kv file beside the conversation is not.
 */
static int test_session_cache_counts_split_directories()
{
    const auto json_dir = make_temp_dir("session-json");
    const auto kv_dir = make_temp_dir("session-kv");
    defer(std::filesystem::remove_all(json_dir));
    defer(std::filesystem::remove_all(kv_dir));

    write_test_file(json_dir / "1.json", std::string(10, 'a'));
    write_test_file(json_dir / "1.kv", std::string(1000, 'x'));
    write_test_file(kv_dir / "1.kv", std::string(100, 'b'));
    write_test_file(kv_dir / "9.kv", std::string(50, 'c'));

    const SessionCacheRef loaded[] = {
        SessionCacheRef{.id = 1, .updated_at = 1, .pinned = true},
    };
    const SessionCachePlan plan = plan_session_cache(json_dir, loaded, 150, kv_dir);
    assertTrue(plan.drop_sessions.empty());
    assertEquals(static_cast<size_t>(1), plan.drop_orphan_kv.size());
    assertEquals(static_cast<SessionID>(9), plan.drop_orphan_kv[0]);
    assertTrue(!plan.still_over);
    return EXIT_SUCCESS;
}

int test_session_disk()
{
    RUN_TEST(test_session_record_round_trip);
    RUN_TEST(test_session_restore_keeps_id_and_drops_generation);
    RUN_TEST(test_session_cache_drops_oldest);
    RUN_TEST(test_session_cache_counts_split_directories);
    return EXIT_SUCCESS;
}

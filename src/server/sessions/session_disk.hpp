#pragma once

#include "api/models.hpp"
#include "api/sessions.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

// Durable session beside <id>.kv. The KV file is the cache. This is the conversation that uses it.
struct SessionRecord
{
    SessionID id = 0;
    std::string system_prompt;
    std::vector<ChatMessage> messages;
    std::vector<ChatTool> tools;
    bool questions = true;
    bool compress_tools = true;
    int max_tokens = -1;
    SessionState state = SessionState::Idle;
    std::vector<ParsedToolCall> pending_tool_calls;
    std::optional<ParsedQuestion> pending_question;
    std::string error;
    std::string error_code;
    int context_used = 0;
    int context_size = 0;
    // Unix seconds. Missing files count as 0 so they are the first ones a size cap removes.
    int64_t updated_at = 0;
};

// One loaded session, as the size cap should see it. Pinned rows are kept even when the
// directory is over the limit: the caller just resumed that session, or the client still owes a reply.
struct SessionCacheRef
{
    SessionID id = 0;
    int64_t updated_at = 0;
    bool pinned = false;
};

struct SessionCachePlan
{
    std::vector<SessionID> drop_sessions;
    std::vector<SessionID> drop_orphan_kv;
    bool still_over = false;
};

// limit 0 leaves the directory alone. Otherwise drop the oldest <id>.json and <id>.kv
// until the directory fits. A json with no loaded row is aged by its file time.
[[nodiscard]] SessionCachePlan plan_session_cache(const std::filesystem::path &dir,
                                                  std::span<const SessionCacheRef> loaded, uint64_t limit);

// <id>.json.tmp, then rename. False leaves the previous file.
[[nodiscard]] bool write_session_record(const std::filesystem::path &dir, const SessionRecord &record);

// Newest id first. A short, foreign, or mismatched file is skipped.
[[nodiscard]] std::vector<SessionRecord> read_session_records(const std::filesystem::path &dir);

void remove_session_record(const std::filesystem::path &dir, SessionID id);

#include "session_disk.hpp"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <string_view>
#include <unordered_map>

#include <nlohmann/json.hpp>

namespace
{

constexpr std::uintmax_t kMaxRecordBytes = 32ull << 20;

bool parse_id(const std::string_view text, SessionID &id)
{
    if (text.empty() || text.size() > 20 || text.find_first_not_of("0123456789") != std::string_view::npos)
    {
        return false;
    }
    try
    {
        std::size_t used = 0;
        const unsigned long long value = std::stoull(std::string(text), &used, 10);
        if (used != text.size())
        {
            return false;
        }
        id = static_cast<SessionID>(value);
        return id != 0;
    }
    catch (const std::exception &)
    {
        return false;
    }
}

std::filesystem::path record_path(const std::filesystem::path &dir, const SessionID id)
{
    return dir / (std::to_string(id) + ".json");
}

std::optional<SessionRecord> record_from_json(const nlohmann::json &json, const SessionID id)
{
    if (json.value("id", SessionID{}) != id)
    {
        return std::nullopt;
    }
    SessionRecord record;
    record.id = id;
    record.system_prompt = json.value("system", std::string{});
    if (const auto messages = json.value("messages", nlohmann::json::array()); messages.is_array())
    {
        record.messages.reserve(messages.size());
        for (const auto &item : messages)
        {
            record.messages.push_back(ChatMessage::from_json(item));
        }
    }
    if (const auto tools = json.value("tools", nlohmann::json::array()); tools.is_array())
    {
        record.tools.reserve(tools.size());
        for (const auto &item : tools)
        {
            record.tools.push_back(ChatTool::from_json(item));
        }
    }
    record.questions = json.value("questions", true);
    record.compress_tools = json.value("compress_tools", true);
    record.max_tokens = json.value("max_tokens", -1);
    record.state = SessionState::to_enum(json.value("state", std::string{"idle"}));
    if (const auto calls = json.value("pending_tool_calls", nlohmann::json::array()); calls.is_array())
    {
        record.pending_tool_calls.reserve(calls.size());
        for (const auto &item : calls)
        {
            record.pending_tool_calls.push_back(ParsedToolCall::from_json(item));
        }
    }
    if (json.contains("pending_question") && json["pending_question"].is_object())
    {
        record.pending_question = ParsedQuestion::from_json(json["pending_question"]);
    }
    record.error = json.value("error", std::string{});
    record.error_code = json.value("error_code", std::string{});
    record.context_used = json.value("context_used", 0);
    record.context_size = json.value("context_size", 0);
    const auto updated = json.value("updated_at", int64_t{0});
    record.updated_at = updated > 0 ? updated : 0;
    return record;
}

nlohmann::json record_to_json(const SessionRecord &record)
{
    nlohmann::json messages = nlohmann::json::array();
    for (const ChatMessage &message : record.messages)
    {
        messages.push_back(message.to_json());
    }
    nlohmann::json tools = nlohmann::json::array();
    for (const ChatTool &tool : record.tools)
    {
        tools.push_back(tool.to_json());
    }
    nlohmann::json calls = nlohmann::json::array();
    for (const ParsedToolCall &call : record.pending_tool_calls)
    {
        calls.push_back(call.to_json());
    }
    nlohmann::json json{{"id", record.id},
                        {"system", record.system_prompt},
                        {"messages", std::move(messages)},
                        {"tools", std::move(tools)},
                        {"questions", record.questions},
                        {"compress_tools", record.compress_tools},
                        {"max_tokens", record.max_tokens},
                        {"state", record.state.to_string()},
                        {"pending_tool_calls", std::move(calls)},
                        {"error", record.error},
                        {"error_code", record.error_code},
                        {"context_used", record.context_used},
                        {"context_size", record.context_size},
                        {"updated_at", record.updated_at}};
    if (record.pending_question)
    {
        json["pending_question"] = record.pending_question->to_json();
    }
    return json;
}

} // namespace

bool write_session_record(const std::filesystem::path &dir, const SessionRecord &record)
{
    if (dir.empty() || record.id == 0)
    {
        return false;
    }
    try
    {
        std::filesystem::create_directories(dir);
        const auto final_path = record_path(dir, record.id);
        const auto tmp_path = dir / (std::to_string(record.id) + ".json.tmp");
        const std::string body = record_to_json(record).dump();
        {
            std::ofstream out(tmp_path, std::ios::binary | std::ios::trunc);
            if (!out)
            {
                return false;
            }
            out.write(body.data(), static_cast<std::streamsize>(body.size()));
            out.flush();
            if (!out)
            {
                std::error_code ignored;
                std::filesystem::remove(tmp_path, ignored);
                return false;
            }
        }
        std::error_code error;
        std::filesystem::rename(tmp_path, final_path, error);
        if (error)
        {
            std::filesystem::remove(tmp_path, error);
            return false;
        }
        return true;
    }
    catch (const std::exception &)
    {
        return false;
    }
}

std::vector<SessionRecord> read_session_records(const std::filesystem::path &dir)
{
    std::vector<SessionRecord> records;
    std::error_code error;
    if (dir.empty() || !std::filesystem::is_directory(dir, error) || error)
    {
        return records;
    }
    for (const auto &entry : std::filesystem::directory_iterator(dir, error))
    {
        if (error || !entry.is_regular_file())
        {
            continue;
        }
        const std::string name = entry.path().filename().string();
        constexpr std::string_view suffix = ".json";
        if (name.size() <= suffix.size() || !name.ends_with(suffix))
        {
            continue;
        }
        SessionID id = 0;
        if (!parse_id(std::string_view(name).substr(0, name.size() - suffix.size()), id))
        {
            continue;
        }
        const auto bytes = entry.file_size(error);
        if (error || bytes > kMaxRecordBytes)
        {
            continue;
        }
        std::ifstream in(entry.path(), std::ios::binary);
        std::string body(static_cast<std::size_t>(bytes), '\0');
        in.read(body.data(), static_cast<std::streamsize>(body.size()));
        if (!in)
        {
            continue;
        }
        try
        {
            if (auto record = record_from_json(nlohmann::json::parse(body), id))
            {
                records.push_back(std::move(*record));
            }
        }
        catch (const std::exception &)
        {
        }
    }
    std::sort(records.begin(), records.end(),
              [](const SessionRecord &left, const SessionRecord &right) { return left.id > right.id; });
    return records;
}

void remove_session_record(const std::filesystem::path &dir, const SessionID id)
{
    if (dir.empty() || id == 0)
    {
        return;
    }
    std::error_code ignored;
    std::filesystem::remove(record_path(dir, id), ignored);
    std::filesystem::remove(dir / (std::to_string(id) + ".json.tmp"), ignored);
}

namespace
{

uint64_t file_bytes(const std::filesystem::path &path)
{
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error) || error)
    {
        return 0;
    }
    const auto bytes = std::filesystem::file_size(path, error);
    return error ? 0 : static_cast<uint64_t>(bytes);
}

int64_t file_mtime(const std::filesystem::path &path)
{
    std::error_code error;
    const auto written = std::filesystem::last_write_time(path, error);
    if (error)
    {
        return 0;
    }
    const auto sys = std::chrono::clock_cast<std::chrono::system_clock>(written);
    return std::chrono::duration_cast<std::chrono::seconds>(sys.time_since_epoch()).count();
}

struct DiskRow
{
    uint64_t json_bytes = 0;
    uint64_t kv_bytes = 0;
    int64_t mtime = 0;
    bool json = false;
    bool kv = false;
};

bool same_directory(const std::filesystem::path &left, const std::filesystem::path &right)
{
    if (left.empty() || right.empty())
    {
        return left.empty() && right.empty();
    }
    std::error_code error;
    if (std::filesystem::exists(left, error) && !error && std::filesystem::exists(right, error) && !error)
    {
        const bool same = std::filesystem::equivalent(left, right, error);
        if (!error)
        {
            return same;
        }
    }
    return left.lexically_normal() == right.lexically_normal();
}

void scan_suffixes(const std::filesystem::path &folder, const bool want_json, const bool want_kv,
                   std::unordered_map<SessionID, DiskRow> &rows)
{
    std::error_code error;
    if (folder.empty() || !std::filesystem::is_directory(folder, error) || error)
    {
        return;
    }
    for (const auto &entry : std::filesystem::directory_iterator(folder, error))
    {
        if (error || !entry.is_regular_file())
        {
            continue;
        }
        const std::string name = entry.path().filename().string();
        const bool json = name.ends_with(".json");
        const bool kv = name.ends_with(".kv");
        if ((json && !want_json) || (kv && !want_kv) || (!json && !kv))
        {
            continue;
        }
        const auto suffix = json ? std::string_view(".json") : std::string_view(".kv");
        SessionID id = 0;
        if (!parse_id(std::string_view(name).substr(0, name.size() - suffix.size()), id))
        {
            continue;
        }
        DiskRow &row = rows[id];
        const uint64_t bytes = file_bytes(entry.path());
        if (json)
        {
            row.json = true;
            row.json_bytes = bytes;
            row.mtime = std::max(row.mtime, file_mtime(entry.path()));
        }
        else
        {
            row.kv = true;
            row.kv_bytes = bytes;
            row.mtime = std::max(row.mtime, file_mtime(entry.path()));
        }
    }
}

} // namespace

SessionCachePlan plan_session_cache(const std::filesystem::path &dir, const std::span<const SessionCacheRef> loaded,
                                    const uint64_t limit, const std::filesystem::path &kv_dir)
{
    SessionCachePlan plan;
    if (limit == 0 || (dir.empty() && kv_dir.empty()))
    {
        return plan;
    }
    const std::filesystem::path kv = kv_dir.empty() ? dir : kv_dir;
    const bool split = !kv_dir.empty() && !same_directory(dir, kv);
    std::unordered_map<SessionID, DiskRow> rows;
    if (split)
    {
        scan_suffixes(dir, true, false, rows);
        scan_suffixes(kv, false, true, rows);
    }
    else
    {
        scan_suffixes(dir.empty() ? kv : dir, true, true, rows);
    }

    struct Candidate
    {
        SessionID id = 0;
        int64_t updated_at = 0;
        uint64_t bytes = 0;
        bool pinned = false;
    };
    std::unordered_map<SessionID, bool> loaded_ids;
    std::vector<Candidate> sessions;
    sessions.reserve(rows.size() + loaded.size());
    for (const SessionCacheRef &ref : loaded)
    {
        if (ref.id == 0 || loaded_ids.contains(ref.id))
        {
            continue;
        }
        loaded_ids.emplace(ref.id, true);
        const auto found = rows.find(ref.id);
        const DiskRow row = found == rows.end() ? DiskRow{} : found->second;
        sessions.push_back(Candidate{ref.id, ref.updated_at, row.json_bytes + row.kv_bytes, ref.pinned});
    }
    std::vector<Candidate> orphans;
    uint64_t total = 0;
    for (const auto &[id, row] : rows)
    {
        total += row.json_bytes + row.kv_bytes;
        if (loaded_ids.contains(id) || row.json)
        {
            if (!loaded_ids.contains(id) && row.json)
            {
                sessions.push_back(Candidate{id, row.mtime, row.json_bytes + row.kv_bytes, false});
            }
            continue;
        }
        if (row.kv)
        {
            orphans.push_back(Candidate{id, row.mtime, row.kv_bytes, false});
        }
    }
    if (total <= limit)
    {
        return plan;
    }

    std::sort(sessions.begin(), sessions.end(), [](const Candidate &left, const Candidate &right) {
        if (left.updated_at != right.updated_at)
        {
            return left.updated_at < right.updated_at;
        }
        return left.id < right.id;
    });
    for (const Candidate &candidate : sessions)
    {
        if (total <= limit)
        {
            break;
        }
        if (candidate.pinned)
        {
            continue;
        }
        plan.drop_sessions.push_back(candidate.id);
        total -= std::min(total, candidate.bytes);
    }
    std::sort(orphans.begin(), orphans.end(), [](const Candidate &left, const Candidate &right) {
        if (left.updated_at != right.updated_at)
        {
            return left.updated_at < right.updated_at;
        }
        return left.id < right.id;
    });
    for (const Candidate &candidate : orphans)
    {
        if (total <= limit)
        {
            break;
        }
        plan.drop_orphan_kv.push_back(candidate.id);
        total -= std::min(total, candidate.bytes);
    }
    plan.still_over = total > limit;
    return plan;
}

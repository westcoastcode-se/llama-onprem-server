#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// One parked sequence. state is the bytes from llama_state_seq_get_data, including its prefix.
struct SessionSnapshot
{
    std::vector<int32_t> tokens;
    std::vector<uint8_t> state;
};

// Bytes held for one snapshot: the state buffer plus the token ids.
[[nodiscard]] uint64_t parked_session_bytes(const SessionSnapshot &snapshot);

struct ParkedBytes
{
    std::string id;
    uint64_t bytes = 0;
};

// keep_incoming is false when the budget is zero or the new session is larger than the whole budget.
// spill is oldest first and is empty when the new session is not kept.
struct MemoryAdmit
{
    bool keep_incoming = false;
    std::vector<std::string> spill;
};

// oldest_first.front() is the session parked longest ago.
[[nodiscard]] MemoryAdmit plan_memory_admit(std::span<const ParkedBytes> oldest_first, uint64_t incoming_bytes,
                                            uint64_t limit_bytes);

struct DiskSessionStat
{
    std::string id;
    uint64_t bytes = 0;
    std::chrono::file_clock::time_point mtime{};
};

struct DiskGcPlan
{
    std::vector<std::string> drop;
};

// max_age_days < 0 disables the plan. A file is listed only when its age is greater than max_age_days
// and the files together are larger than max_bytes. Oldest first. Newer files are left in place
// even when the directory is still over the limit.
[[nodiscard]] DiskGcPlan plan_session_disk_gc(std::span<const DiskSessionStat> files, int max_age_days,
                                              uint64_t max_bytes, std::chrono::file_clock::time_point now);

// Parked sessions kept in the process. The worker thread is the only caller.
// front of the list was parked first.
class SessionMemory
{
  public:
    SessionMemory() = default;
    explicit SessionMemory(uint64_t limit_bytes);

    [[nodiscard]] uint64_t limit_bytes() const
    {
        return limit_bytes_;
    }

    [[nodiscard]] uint64_t used_bytes() const
    {
        return used_bytes_;
    }

    [[nodiscard]] bool contains(std::string_view id) const
    {
        return find(id) != nullptr;
    }

    [[nodiscard]] const SessionSnapshot *find(std::string_view id) const;

    [[nodiscard]] std::optional<size_t> token_count(std::string_view id) const;

    // Drops the session from RAM and returns it. Empty when it was not parked here.
    [[nodiscard]] std::optional<SessionSnapshot> take(std::string_view id);

    void erase(std::string_view id);

    // Moves id to the newest end. Replaces a session already stored under id.
    void insert(std::string id, SessionSnapshot snapshot);

    // replacing is left out of the budget, so parking the same id again does not spill itself.
    [[nodiscard]] MemoryAdmit plan(uint64_t incoming_bytes, std::string_view replacing) const;

    [[nodiscard]] std::vector<std::string> ids_oldest_first() const;

  private:
    struct Entry
    {
        std::string id;
        SessionSnapshot snapshot;
        uint64_t bytes = 0;
    };

    [[nodiscard]] const Entry *find_entry(std::string_view id) const;

    uint64_t limit_bytes_ = 0;
    uint64_t used_bytes_ = 0;
    std::vector<Entry> entries_;
};

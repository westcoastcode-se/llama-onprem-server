#include "session_park.hpp"

#include <algorithm>
#include <limits>
#include <ranges>
#include <utility>

uint64_t parked_session_bytes(const SessionSnapshot &snapshot)
{
    const uint64_t state_bytes = snapshot.state.size();
    if (snapshot.tokens.size() > std::numeric_limits<uint64_t>::max() / sizeof(int32_t))
    {
        return std::numeric_limits<uint64_t>::max();
    }
    const uint64_t token_bytes = snapshot.tokens.size() * sizeof(int32_t);
    if (state_bytes > std::numeric_limits<uint64_t>::max() - token_bytes)
    {
        return std::numeric_limits<uint64_t>::max();
    }
    return state_bytes + token_bytes;
}

MemoryAdmit plan_memory_admit(const std::span<const ParkedBytes> oldest_first, const uint64_t incoming_bytes,
                              const uint64_t limit_bytes)
{
    MemoryAdmit admit;
    if (limit_bytes == 0 || incoming_bytes == 0 || incoming_bytes > limit_bytes)
    {
        return admit;
    }
    uint64_t used = 0;
    for (const ParkedBytes &parked : oldest_first)
    {
        if (used > std::numeric_limits<uint64_t>::max() - parked.bytes)
        {
            used = std::numeric_limits<uint64_t>::max();
            break;
        }
        used += parked.bytes;
    }
    const uint64_t room = limit_bytes - incoming_bytes;
    std::size_t index = 0;
    while (index < oldest_first.size() && used > room)
    {
        admit.spill.push_back(oldest_first[index].id);
        if (used >= oldest_first[index].bytes)
        {
            used -= oldest_first[index].bytes;
        }
        else
        {
            used = 0;
        }
        ++index;
    }
    admit.keep_incoming = used <= room;
    return admit;
}

DiskGcPlan plan_session_disk_gc(const std::span<const DiskSessionStat> files, const int max_age_days,
                                const uint64_t max_bytes, const std::chrono::file_clock::time_point now)
{
    DiskGcPlan plan;
    if (max_age_days < 0)
    {
        return plan;
    }
    uint64_t total = 0;
    for (const DiskSessionStat &file : files)
    {
        if (total > std::numeric_limits<uint64_t>::max() - file.bytes)
        {
            total = std::numeric_limits<uint64_t>::max();
            break;
        }
        total += file.bytes;
    }
    if (total <= max_bytes)
    {
        return plan;
    }
    const auto max_age = std::chrono::hours{24} * static_cast<std::chrono::hours::rep>(max_age_days);
    std::vector<std::size_t> old;
    old.reserve(files.size());
    for (std::size_t index = 0; index < files.size(); ++index)
    {
        const DiskSessionStat &file = files[index];
        if (file.mtime >= now)
        {
            continue;
        }
        if (now - file.mtime > max_age)
        {
            old.push_back(index);
        }
    }
    std::ranges::sort(old, [&](const std::size_t left, const std::size_t right) {
        if (files[left].mtime != files[right].mtime)
        {
            return files[left].mtime < files[right].mtime;
        }
        return files[left].id < files[right].id;
    });
    for (const std::size_t index : old)
    {
        if (total <= max_bytes)
        {
            break;
        }
        const DiskSessionStat &file = files[index];
        plan.drop.push_back(file.id);
        if (total >= file.bytes)
        {
            total -= file.bytes;
        }
        else
        {
            total = 0;
        }
    }
    return plan;
}

SessionMemory::SessionMemory(const uint64_t limit_bytes) : limit_bytes_(limit_bytes)
{
}

const SessionMemory::Entry *SessionMemory::find_entry(const std::string_view id) const
{
    for (const Entry &entry : entries_)
    {
        if (entry.id == id)
        {
            return &entry;
        }
    }
    return nullptr;
}

const SessionSnapshot *SessionMemory::find(const std::string_view id) const
{
    const Entry *entry = find_entry(id);
    return entry == nullptr ? nullptr : &entry->snapshot;
}

std::optional<size_t> SessionMemory::token_count(const std::string_view id) const
{
    const SessionSnapshot *snapshot = find(id);
    if (snapshot == nullptr)
    {
        return std::nullopt;
    }
    return snapshot->tokens.size();
}

std::optional<SessionSnapshot> SessionMemory::take(const std::string_view id)
{
    const auto it = std::ranges::find_if(entries_, [&](const Entry &entry) { return entry.id == id; });
    if (it == entries_.end())
    {
        return std::nullopt;
    }
    SessionSnapshot snapshot = std::move(it->snapshot);
    if (used_bytes_ >= it->bytes)
    {
        used_bytes_ -= it->bytes;
    }
    else
    {
        used_bytes_ = 0;
    }
    entries_.erase(it);
    return snapshot;
}

void SessionMemory::erase(const std::string_view id)
{
    (void)take(id);
}

void SessionMemory::insert(std::string id, SessionSnapshot snapshot)
{
    erase(id);
    Entry entry;
    entry.bytes = parked_session_bytes(snapshot);
    entry.id = std::move(id);
    entry.snapshot = std::move(snapshot);
    if (used_bytes_ > std::numeric_limits<uint64_t>::max() - entry.bytes)
    {
        used_bytes_ = std::numeric_limits<uint64_t>::max();
    }
    else
    {
        used_bytes_ += entry.bytes;
    }
    entries_.push_back(std::move(entry));
}

MemoryAdmit SessionMemory::plan(const uint64_t incoming_bytes, const std::string_view replacing) const
{
    std::vector<ParkedBytes> held;
    held.reserve(entries_.size());
    for (const Entry &entry : entries_)
    {
        if (entry.id == replacing)
        {
            continue;
        }
        held.push_back(ParkedBytes{entry.id, entry.bytes});
    }
    return plan_memory_admit(held, incoming_bytes, limit_bytes_);
}

std::vector<std::string> SessionMemory::ids_oldest_first() const
{
    std::vector<std::string> ids;
    ids.reserve(entries_.size());
    for (const Entry &entry : entries_)
    {
        ids.push_back(entry.id);
    }
    return ids;
}

#pragma once

#include "api/messages.hpp"
#include "api/sessions.hpp"
#include "jobs/jobs.hpp"
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

struct Session
{
    using Key = std::string;

    Key id;
    std::string system;
    std::vector<ChatMessage> messages;
    std::string active_job_key;
    std::chrono::steady_clock::time_point created_at = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point last_active = std::chrono::steady_clock::now();

    mutable std::mutex mutex;

    void touch()
    {
        last_active = std::chrono::steady_clock::now();
    }

    [[nodiscard]] SessionResponse to_response() const
    {
        // caller must hold mutex or own exclusive access
        SessionResponse r;
        r.id = id;
        r.system = system;
        r.messages = messages;
        r.active_job_key = active_job_key;
        r.state = active_job_key.empty() ? "idle" : "generating";
        return r;
    }
};

/**
 * Server-side chat sessions: durable message history, turns go through Jobs.
 * Max ~2 concurrent clients; sessions themselves are cheap metadata.
 */
class Sessions
{
  public:
    static constexpr size_t kMaxSessions = 32;
    static constexpr std::chrono::seconds kIdleTtl{3600};

    explicit Sessions(Jobs &jobs);

    std::shared_ptr<Session> create(CreateSessionRequest req);

    std::shared_ptr<Session> get(const Session::Key &id);

    bool destroy(const Session::Key &id);

    /**
     * Append a user turn and enqueue a generation job.
     * @return job key, or nullopt if queue full.
     * Throws BadRequest / Busy-style via optional error out... use expected-like:
     * returns nullopt only for queue full; throws for logical errors via exception types.
     */
    std::optional<std::string> post_message(const Session::Key &id, SessionMessageRequest msg);

    void gc();

  private:
    Jobs &jobs_;
    std::mutex mutex_;
    std::unordered_map<Session::Key, std::shared_ptr<Session>> sessions_;
    std::atomic<uint64_t> id_counter_{1};

    Session::Key next_id();
    void unsafe_gc();
};

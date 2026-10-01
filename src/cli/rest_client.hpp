#pragma once

#include "api/errors.hpp"
#include "api/models.hpp"
#include "common/log.hpp"

#include <format>
#include <functional>
#include <httplib.h>
#include <stdexcept>
#include <utility>

/**
 * Thin HTTP helper around callisto_server REST endpoints (/v1/...).
 */
inline constexpr int kRestConnectSeconds = 5;
inline constexpr int kRestProbeSeconds = 1;

class RestClient
{
  public:
    using TokenCallback = std::move_only_function<bool(const std::string &piece)>;
    using ContextCallback = std::move_only_function<void(int used, int size)>;

    struct ClientError : std::runtime_error
    {
        int status = 0;
        std::string body;
        std::string code;

        ClientError(const int status, std::string body, const std::string &what, std::string code = {})
            : std::runtime_error(what), status(status), body(std::move(body)), code(std::move(code))
        {
        }
    };

    RestClient(std::string host, const int port);

    [[nodiscard]] std::string base_url() const;

    [[nodiscard]] const std::string &host() const;

    [[nodiscard]] int port() const;

    void set_connection_timeout(int seconds);

    /** Interrupt an in-flight request (e.g. token stream) from another thread. */
    void stop();

    /**
     * Do a health check against the server
     *
     * @return true if the server is healthy
     */
    bool health();

    // Short connect timeout so a dead server does not stall startup or /model.
    bool probe();

    /**
     * Create a new session
     *
     * @param body The creation request
     * @return Information on the created session
     */
    SessionResponse create_session(const CreateSessionRequest &body);

    /**
     * Get information on a specific session
     *
     * @param id The unique session id
     * @return Information on the session
     */
    /**
     * Copy the session's messages and prompt into a new session.
     * The child does not receive the sub_agent tool.
     */
    SessionResponse snapshot_session(const SessionID &id);

    SessionResponse get_session(const SessionID &id);

    /**
     * Delete the session with the supplied id. This will cleanup all of it's resources
     * on the server and abort any running chat request if running
     *
     * @param id The session id
     */
    void delete_session(const SessionID &id);

    /**
     * Post a new chat message to the supplied session
     *
     * @param session_id The unique session id
     * @param request The session creation request
     * @return Information on the chat message
     */
    SessionMessageResponse post_message(const SessionID &session_id, const SessionMessageRequest &request);

    // tool_results stays JSON. The server names the calls, and each call's output is open-ended.
    SessionMessageResponse post_tool_results(const SessionID &session_id, const nlohmann::json &tool_results);

    MessageStatusResponse get_job(const SessionID session_id, const JobKey key);

    /**
     * Try to cancel a non-finished job
     *
     * @param session_id The session id
     * @param key The job key
     * @return true if the job was cancelled successfully
     */
    bool cancel_job(const SessionID session_id, const JobKey key);

    /**
     * Stream NDJSON token lines from GET /v1/sessions/:id/jobs/:key/tokens.
     * Invokes cb for each tokens field; returns concatenated text.
     *
     * Note: ContentReceiver must keep returning true until the server closes the
     * chunked body. Returning false makes cpp-httplib treat the call as
     * Error::Canceled with a null Result (looks like "no response").
     */
    std::string stream_tokens(const SessionID session_id, const JobKey key, TokenCallback cb = {},
                         ContextCallback on_context = {});

  private:
    std::string base_host_;
    int port_;
    httplib::Client cli_;

    nlohmann::json request_json(const char *method, const std::string &path, std::optional<nlohmann::json> body, int expect_status);
};

#pragma once

#include "../api/errors.hpp"
#include "../api/models.hpp"
#include "../common/std.hpp"
#include "common/log.hpp"

#include <format>
#include <functional>
#include <httplib.h>
#include <stdexcept>
#include <utility>

/**
 * Thin HTTP helper around callisto_server REST endpoints (/v1/...).
 */
class RestClient
{
  public:
    using TokenCallback = std::move_only_function<bool(const string &piece)>;

    struct ClientError : std::runtime_error
    {
        int status = 0;
        string body;
        string code;

        ClientError(const int status, string body, const string &what, string code = {})
            : std::runtime_error(what), status(status), body(std::move(body)), code(std::move(code))
        {
        }
    };

    RestClient(string host, const int port) : base_host_(std::move(host)), port_(port), cli_(base_host_, port_)
    {
        cli_.set_connection_timeout(5, 0);
        cli_.set_read_timeout(600, 0);
        cli_.set_write_timeout(30, 0);
        cli_.set_keep_alive(true);
    }

    [[nodiscard]] std::string base_url() const
    {
        return std::format("http://{}:{}", base_host_, port_);
    }

    [[nodiscard]] const std::string &host() const
    {
        return base_host_;
    }

    [[nodiscard]] int port() const
    {
        return port_;
    }

    /** Interrupt an in-flight request (e.g. token stream) from another thread. */
    void stop()
    {
        cli_.stop();
    }

    /**
     * Do a health check against the server
     *
     * @return true if the server is healthy
     */
    bool health()
    {
        auto res = cli_.Get("/health");
        return res && res->status == 200;
    }

    /**
     * Create a new session
     *
     * @param body The creation request
     * @return Information on the created session
     */
    SessionResponse create_session(const CreateSessionRequest &body)
    {
        body.validate();
        auto resp = SessionResponse::from_json(request_json("POST", "/v1/sessions", body.to_json(), 201));
        resp.validate();
        return resp;
    }

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
    SessionResponse snapshot_session(const SessionID &id)
    {
        return SessionResponse::from_json(
            request_json("POST", "/v1/sessions/" + std::to_string(id) + "/snapshots", json::object(), 201));
    }

    SessionResponse get_session(const SessionID &id)
    {
        return SessionResponse::from_json(request_json("GET", "/v1/sessions/" + std::to_string(id), std::nullopt, 200));
    }

    /**
     * Delete the session with the supplied id. This will cleanup all of it's resources
     * on the server and abort any running chat request if running
     *
     * @param id The session id
     */
    void delete_session(const SessionID &id)
    {
        auto res = cli_.Delete("/v1/sessions/" + std::to_string(id));
        if (!res)
        {
            throw ClientError(0, "", std::format("delete session failed: no response from {}", base_url()));
        }
        if (res->status != 200 && res->status != 404)
        {
            throw ClientError(res->status, res->body, std::format("DELETE /v1/sessions/{} failed", id));
        }
    }

    /**
     * Post a new chat message to the supplied session
     *
     * @param session_id The unique session id
     * @param request The session creation request
     * @return Information on the chat message
     */
    SessionMessageResponse post_message(const SessionID &session_id, const SessionMessageRequest &request)
    {
        request.validate();
        auto resp = SessionMessageResponse::from_json(
            request_json("POST", "/v1/sessions/" + std::to_string(session_id) + "/messages", request.to_json(), 200));
        resp.validate();
        return resp;
    }

    json post_tool_results(const SessionID &session_id, const json &tool_results)
    {
        return request_json("POST", "/v1/sessions/" + std::to_string(session_id) + "/tools",
                            json{{"tool_results", tool_results}}, 200);
    }

    json get_job(const SessionID session_id, const JobKey key)
    {
        return request_json("GET", "/v1/sessions/" + std::to_string(session_id) + "/jobs/" +
            std::to_string(key), std::nullopt, 200);
    }

    /**
     * Try to cancel a non-finished job
     *
     * @param session_id The session id
     * @param key The job key
     * @return true if the job was cancelled successfully
     */
    bool cancel_job(const SessionID session_id, const JobKey key)
    {
        auto res = cli_.Delete("/v1/sessions/" + std::to_string(session_id) + "/jobs/" + std::to_string(key));
        return res && res->status == 200;
    }

    /**
     * Stream NDJSON token lines from GET /v1/sessions/:id/jobs/:key/tokens.
     * Invokes cb for each tokens field; returns concatenated text.
     *
     * Note: ContentReceiver must keep returning true until the server closes the
     * chunked body. Returning false makes cpp-httplib treat the call as
     * Error::Canceled with a null Result (looks like "no response").
     */
    string stream_tokens(const SessionID session_id, const JobKey key, TokenCallback cb = {})
    {
        std::string accumulated;
        std::string line_buf;
        std::string stream_error;
        std::string stream_code;
        bool is_done = false;
        bool client_cancel = false;

        auto handle_line = [&](std::string_view line) -> bool {
            if (!line.empty() && line.back() == '\r')
            {
                line.remove_suffix(1);
            }
            if (line.empty())
            {
                return true;
            }
            json parsed = json::parse(line, nullptr, false);
            if (parsed.is_discarded())
            {
                stream_error = "token stream returned invalid json";
                return false;
            }
            const auto t = MessageTokensResponse::from_json(parsed);
            accumulated += t.tokens;
            if (cb && !t.tokens.empty() && !cb(t.tokens))
            {
                client_cancel = true;
                return false;
            }
            if (t.done)
            {
                is_done = true;
                if (t.state == "error")
                {
                    stream_error = t.error.empty() ? "generation failed" : t.error;
                    stream_code = t.error_code;
                }
                return false;
            }
            return true;
        };

        // Chunks are not NDJSON lines. Buffer until '\n' before parsing.
        auto res = cli_.Get("/v1/sessions/" + std::to_string(session_id) + "/jobs/" +
            std::to_string(key) + "/tokens",
                            [&](const char * const data, const size_t len) {
                                if (client_cancel || is_done)
                                {
                                    return false;
                                }
                                line_buf.append(data, len);
                                size_t start = 0;
                                while (true)
                                {
                                    const auto nl = line_buf.find('\n', start);
                                    if (nl == std::string::npos)
                                    {
                                        line_buf.erase(0, start);
                                        return true;
                                    }
                                    const bool keep_going = handle_line(std::string_view(line_buf).substr(start, nl - start));
                                    start = nl + 1;
                                    if (!keep_going)
                                    {
                                        line_buf.clear();
                                        return false;
                                    }
                                }
                            });

        if (!res)
        {
            // Successful end-of-stream via ContentReceiver cancel is not used; real
            // connection failures still surface here. If we already saw done=true,
            // treat as success (defensive for older httplib edge cases).
            if (is_done && !client_cancel && stream_error.empty())
            {
                return accumulated;
            }
            if (!stream_error.empty() && !client_cancel)
            {
                throw ClientError(0, stream_error, stream_error, stream_code);
            }
            const auto err = res.error();
            throw ClientError(0, "", std::format("token stream failed: no response for job {} (httplib error {})", key,
                                                 static_cast<int>(err)));
        }
        if (res->status != 200)
        {
            throw ClientError(res->status, res->body,
                              std::format("GET /v1/sessions/{}/jobs/{}/tokens failed: {}", session_id, key, res->body));
        }
        if (!stream_error.empty() && !client_cancel)
        {
            throw ClientError(0, stream_error, stream_error, stream_code);
        }
        return accumulated;
    }

  private:
    std::string base_host_;
    int port_;
    httplib::Client cli_;

    json request_json(const char *method, const std::string &path, std::optional<json> body, int expect_status)
    {
        httplib::Result res;
        const std::string payload = body ? body->dump() : "";
        const std::string_view verb = method;
        if (verb == "GET")
        {
            res = cli_.Get(path);
        }
        else if (verb == "POST")
        {
            res = cli_.Post(path, payload, "application/json");
        }
        else if (verb == "DELETE")
        {
            res = cli_.Delete(path);
        }
        else
        {
            throw ClientError(0, "", std::format("unsupported method {}", method));
        }

        if (!res)
        {
            throw ClientError(0, "", std::format("{} {} failed: no response from {}", method, path, base_url()));
        }
        if (res->status != expect_status)
        {
            std::string msg = std::format("{} {} → HTTP {}", method, path, res->status);
            try
            {
                const auto error_resp = ErrorResponse::from_json(json::parse(res->body));
                msg += std::format(": error_code({}) {}", error_resp.error_code, error_resp.message);
            }
            catch (...)
            {
                if (!res->body.empty())
                {
                    msg += ": " + res->body;
                }
            }
            throw ClientError(res->status, res->body, msg);
        }
        if (res->body.empty())
        {
            return json::object();
        }
        return json::parse(res->body);
    }
};

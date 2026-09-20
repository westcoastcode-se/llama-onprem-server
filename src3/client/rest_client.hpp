#pragma once

#include "../api/models.hpp"
#include "../common/std.hpp"

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
    using TokenCallback = std::function<bool(string_view piece)>;

    struct ClientError : std::runtime_error
    {
        int status = 0;
        string body;

        ClientError(const int status, string body, const string &what)
            : std::runtime_error(what), status(status), body(std::move(body))
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
        return "http://" + base_host_ + ":" + std::to_string(port_);
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
            throw ClientError(0, "", "delete session failed: no response from " + base_url());
        }
        if (res->status != 200 && res->status != 404)
        {
            throw ClientError(res->status, res->body, "DELETE /v1/sessions/" + std::to_string(id) + " failed");
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

    json get_job(const SessionID &session_id, const JobKey &key)
    {
        return request_json("GET", "/v1/sessions/" + std::to_string(session_id) + "/jobs/" + key, std::nullopt, 200);
    }

    /**
     * Try to cancel a non-finished job
     *
     * @param session_id The session id
     * @param key The job key
     * @return true if the job was cancelled successfully
     */
    bool cancel_job(const SessionID &session_id, const JobKey &key)
    {
        auto res = cli_.Delete("/v1/sessions/" + std::to_string(session_id) + "/jobs/" + key);
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
    std::string stream_tokens(const SessionID &session_id, const JobKey &key, const TokenCallback &cb = nullptr)
    {
        std::string accumulated;
        std::string line_buf;
        bool saw_done = false;
        bool client_cancel = false;

        auto res = cli_.Get("/v1/sessions/" + std::to_string(session_id) + "/jobs/" + key + "/tokens",
                            [&](const char *data, size_t len) {
                                if (saw_done)
                                {
                                    // Drain any trailing bytes after the terminal NDJSON line.
                                    return true;
                                }
                                line_buf.append(data, len);
                                for (;;)
                                {
                                    auto pos = line_buf.find('\n');
                                    if (pos == std::string::npos)
                                    {
                                        break;
                                    }
                                    std::string line = line_buf.substr(0, pos);
                                    line_buf.erase(0, pos + 1);
                                    if (line.empty())
                                    {
                                        continue;
                                    }
                                    // Strip optional CR from CRLF-framed lines.
                                    if (!line.empty() && line.back() == '\r')
                                    {
                                        line.pop_back();
                                    }
                                    json j = json::parse(line, nullptr, false);
                                    if (j.is_discarded())
                                    {
                                        continue;
                                    }
                                    const bool done = j.value("done", false);
                                    std::string tokens;
                                    if (j.contains("tokens"))
                                    {
                                        if (j["tokens"].is_string())
                                        {
                                            tokens = j["tokens"].get<std::string>();
                                        }
                                        else if (j["tokens"].is_array())
                                        {
                                            for (const auto &t : j["tokens"])
                                            {
                                                if (t.is_string())
                                                {
                                                    tokens += t.get<std::string>();
                                                }
                                            }
                                        }
                                    }
                                    if (!tokens.empty())
                                    {
                                        accumulated += tokens;
                                        if (cb && !cb(tokens))
                                        {
                                            client_cancel = true;
                                            return false; // intentional cancel from callback
                                        }
                                    }
                                    if (done)
                                    {
                                        saw_done = true;
                                        // Keep returning true so httplib finishes the chunked response cleanly.
                                        return true;
                                    }
                                }
                                return true;
                            });

        if (!res)
        {
            // Successful end-of-stream via ContentReceiver cancel is not used; real
            // connection failures still surface here. If we already saw done=true,
            // treat as success (defensive for older httplib edge cases).
            if (saw_done && !client_cancel)
            {
                return accumulated;
            }
            const auto err = res.error();
            throw ClientError(0, "",
                        "token stream failed: no response for job " + key + " (httplib error " +
                            std::to_string(static_cast<int>(err)) + ")");
        }
        if (res->status != 200)
        {
            throw ClientError(res->status, res->body,
                        "GET /v1/sessions/" + std::to_string(session_id) + "/jobs/" + key + "/tokens failed");
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
        if (std::string(method) == "GET")
        {
            res = cli_.Get(path);
        }
        else if (std::string(method) == "POST")
        {
            res = cli_.Post(path, payload, "application/json");
        }
        else if (std::string(method) == "DELETE")
        {
            res = cli_.Delete(path);
        }
        else
        {
            throw ClientError(0, "", std::string("unsupported method ") + method);
        }

        if (!res)
        {
            throw ClientError(0, "", std::string(method) + " " + path + " failed: no response from " + base_url());
        }
        if (res->status != expect_status)
        {
            std::string msg = std::string(method) + " " + path + " → HTTP " + std::to_string(res->status);
            try
            {
                const auto error_resp = ErrorResponse::from_json(json::parse(res->body));
                msg += ": error_code(" + std::to_string(error_resp.error_code) + ") ";
                msg += error_resp.message;
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

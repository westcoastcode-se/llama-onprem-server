#pragma once

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

    struct Error : std::runtime_error
    {
        int status = 0;
        string body;

        Error(int status, string body, const string &what)
            : std::runtime_error(what), status(status), body(std::move(body))
        {
        }
    };

    RestClient(string host, const int port)
        : base_host_(std::move(host)), port_(port), cli_(base_host_, port_)
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

    /** GET /health → true if body is OK */
    bool health()
    {
        auto res = cli_.Get("/health");
        return res && res->status == 200;
    }

    json create_session(const json &body)
    {
        return request_json("POST", "/v1/sessions", body, 201);
    }

    json get_session(const std::string &id)
    {
        return request_json("GET", "/v1/sessions/" + id, std::nullopt, 200);
    }

    void delete_session(const std::string &id)
    {
        auto res = cli_.Delete("/v1/sessions/" + id);
        if (!res)
        {
            throw Error(0, "", "delete session failed: no response from " + base_url());
        }
        if (res->status != 200 && res->status != 404)
        {
            throw Error(res->status, res->body, "DELETE /v1/sessions/" + id + " failed");
        }
    }

    /** POST message → {session_id,key} */
    json post_message(const std::string &session_id, const std::string &content,
                      const std::string &role = "user")
    {
        return request_json("POST", "/v1/sessions/" + session_id + "/messages",
                            json{{"content", content}, {"role", role}}, 200);
    }

    json post_tool_results(const std::string &session_id, const json &tool_results)
    {
        return request_json("POST", "/v1/sessions/" + session_id + "/tools",
                            json{{"tool_results", tool_results}}, 200);
    }

    /** One-shot generation without a session. */
    json post_messages(const json &body)
    {
        return request_json("POST", "/v1/messages", body, 200);
    }

    json get_job(const std::string &key)
    {
        return request_json("GET", "/v1/messages/" + key, std::nullopt, 200);
    }

    bool cancel_job(const std::string &key)
    {
        auto res = cli_.Delete("/v1/messages/" + key);
        return res && res->status == 200;
    }

    /**
     * Stream NDJSON token lines from GET /v1/messages/:key/tokens.
     * Invokes cb for each tokens field; returns concatenated text.
     *
     * Note: ContentReceiver must keep returning true until the server closes the
     * chunked body. Returning false makes cpp-httplib treat the call as
     * Error::Canceled with a null Result (looks like "no response").
     */
    std::string stream_tokens(const std::string &key, const TokenCallback &cb = nullptr)
    {
        std::string accumulated;
        std::string line_buf;
        bool saw_done = false;
        bool client_cancel = false;

        auto res = cli_.Get(
            "/v1/messages/" + key + "/tokens",
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
            throw Error(0, "", "token stream failed: no response for job " + key +
                                   " (httplib error " + std::to_string(static_cast<int>(err)) + ")");
        }
        if (res->status != 200)
        {
            throw Error(res->status, res->body, "GET /v1/messages/" + key + "/tokens failed");
        }
        return accumulated;
    }

  private:
    std::string base_host_;
    int port_;
    httplib::Client cli_;

    json request_json(const char *method, const std::string &path, std::optional<json> body,
                      int expect_status)
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
            throw Error(0, "", std::string("unsupported method ") + method);
        }

        if (!res)
        {
            throw Error(0, "", std::string(method) + " " + path + " failed: no response from " +
                                   base_url());
        }
        if (res->status != expect_status)
        {
            std::string msg = std::string(method) + " " + path + " → HTTP " + std::to_string(res->status);
            try
            {
                auto ej = json::parse(res->body);
                if (ej.contains("error") && ej["error"].is_object())
                {
                    msg += ": " + ej["error"].value("message", ej["error"].dump());
                }
                else if (ej.contains("message"))
                {
                    msg += ": " + ej.value("message", res->body);
                }
                else if (!res->body.empty())
                {
                    msg += ": " + res->body;
                }
            }
            catch (...)
            {
                if (!res->body.empty())
                {
                    msg += ": " + res->body;
                }
            }
            throw Error(res->status, res->body, msg);
        }
        if (res->body.empty())
        {
            return json::object();
        }
        return json::parse(res->body);
    }
};

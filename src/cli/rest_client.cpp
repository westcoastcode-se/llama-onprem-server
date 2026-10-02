#include "cli/rest_client.hpp"

RestClient::RestClient(std::string host, const int port) : base_host_(std::move(host)), port_(port), cli_(base_host_, port_)
{
    cli_.set_connection_timeout(kRestConnectSeconds, 0);
    cli_.set_read_timeout(600, 0);
    cli_.set_write_timeout(30, 0);
    cli_.set_keep_alive(true);
}

[[nodiscard]] std::string RestClient::base_url() const
{
    return std::format("http://{}:{}", base_host_, port_);
}

[[nodiscard]] const std::string &RestClient::host() const
{
    return base_host_;
}

[[nodiscard]] int RestClient::port() const
{
    return port_;
}

void RestClient::set_connection_timeout(int seconds)
{
    cli_.set_connection_timeout(seconds, 0);
}

void RestClient::stop()
{
    cli_.stop();
}

bool RestClient::health()
{
    auto res = cli_.Get("/health");
    return res && res->status == 200;
}

bool RestClient::probe()
{
    set_connection_timeout(kRestProbeSeconds);
    const bool up = health();
    set_connection_timeout(kRestConnectSeconds);
    return up;
}

SessionResponse RestClient::create_session(const CreateSessionRequest &body)
{
    body.validate();
    auto resp = SessionResponse::from_json(request_json("POST", "/v1/sessions", body.to_json(), 201));
    resp.validate();
    return resp;
}

SessionResponse RestClient::snapshot_session(const SessionID &id)
{
    return SessionResponse::from_json(
        request_json("POST", "/v1/sessions/" + std::to_string(id) + "/snapshots", nlohmann::json::object(), 201));
}

SessionResponse RestClient::get_session(const SessionID &id)
{
    return SessionResponse::from_json(request_json("GET", "/v1/sessions/" + std::to_string(id), std::nullopt, 200));
}

std::vector<SessionResponse> RestClient::list_sessions()
{
    const nlohmann::json body = request_json("GET", "/v1/sessions", std::nullopt, 200);
    return SessionListResponse::from_json(body).sessions;
}

std::vector<ChatMessage> RestClient::get_messages(const SessionID &id)
{
    const nlohmann::json body =
        request_json("GET", "/v1/sessions/" + std::to_string(id) + "/messages", std::nullopt, 200);
    return SessionMessagesResponse::from_json(body).messages;
}

void RestClient::delete_session(const SessionID &id)
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

SessionMessageResponse RestClient::post_message(const SessionID &session_id, const SessionMessageRequest &request)
{
    request.validate();
    auto resp = SessionMessageResponse::from_json(
        request_json("POST", "/v1/sessions/" + std::to_string(session_id) + "/messages", request.to_json(), 200));
    resp.validate();
    return resp;
}

SessionMessageResponse RestClient::post_tool_results(const SessionID &session_id, const nlohmann::json &tool_results)
{
    auto resp = SessionMessageResponse::from_json(request_json(
        "POST", "/v1/sessions/" + std::to_string(session_id) + "/tools", nlohmann::json{{"tool_results", tool_results}}, 200));
    resp.validate();
    return resp;
}

MessageStatusResponse RestClient::get_job(const SessionID session_id, const JobKey key)
{
    auto resp = MessageStatusResponse::from_json(request_json(
        "GET", "/v1/sessions/" + std::to_string(session_id) + "/jobs/" + std::to_string(key), std::nullopt, 200));
    resp.validate();
    return resp;
}

bool RestClient::cancel_job(const SessionID session_id, const JobKey key)
{
    auto res = cli_.Delete("/v1/sessions/" + std::to_string(session_id) + "/jobs/" + std::to_string(key));
    return res && res->status == 200;
}

std::string RestClient::stream_tokens(const SessionID session_id, const JobKey key, TokenCallback cb,
                         ContextCallback on_context)
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
        nlohmann::json parsed = nlohmann::json::parse(line, nullptr, false);
        if (parsed.is_discarded())
        {
            stream_error = "token stream returned invalid json";
            return false;
        }
        const auto t = MessageTokensResponse::from_json(parsed);
        if (on_context && t.context_size > 0)
        {
            on_context(t.context_used, t.context_size);
        }
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

nlohmann::json RestClient::request_json(const char *method, const std::string &path, std::optional<nlohmann::json> body, int expect_status)
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
            const auto error_resp = ErrorResponse::from_json(nlohmann::json::parse(res->body));
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
        return nlohmann::json::object();
    }
    return nlohmann::json::parse(res->body);
}


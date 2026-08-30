#pragma once

#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include "common/color.hpp"
#include "common/net.hpp"
#include "common/protocol.hpp"

// Token callback returning bool: return true to continue, false to abort generation
using AgentTokenCallback = std::function<bool(std::string_view piece)>;

class IAgentBackend {
public:
    virtual ~IAgentBackend() = default;

    virtual std::string chat(std::span<const Protocol::ChatMessage> messages,
                             AgentTokenCallback token_cb = nullptr,
                             float temp_override = -1.0f) = 0;

    virtual std::string generate(std::string_view prompt,
                                 AgentTokenCallback token_cb = nullptr,
                                 float temp_override = -1.0f) = 0;

    virtual int get_context_size() = 0;
    virtual int get_used_context() = 0;
    virtual void reset_context() = 0;
    virtual std::string get_model_name() const { return ""; }
    virtual void refresh_context_info() {}
};

class RemoteAgentBackend : public IAgentBackend {
public:
    explicit RemoteAgentBackend(std::unique_ptr<TcpSocket> sock,
                                std::string model_name = "",
                                int n_ctx = 0,
                                int used_ctx = 0)
        : sock_(std::move(sock)), model_name_(std::move(model_name)), n_ctx_(n_ctx), used_ctx_(used_ctx) {}

    bool is_valid() const {
        return sock_ && sock_->is_valid();
    }

    TcpSocket * socket() {
        return sock_.get();
    }

    void close() {
        if (sock_) {
            sock_->close();
        }
    }

    std::string chat(std::span<const Protocol::ChatMessage> messages,
                     AgentTokenCallback token_cb = nullptr,
                     float temp_override = -1.0f) override {
        if (!sock_ || !sock_->is_valid()) return "";

        nlohmann::json chat_req = {
            {"type", "chat"},
            {"messages", Protocol::messages_to_json(messages)},
            {"temperature", temp_override},
            {"stream", true}
        };

        if (!sock_->send_json(chat_req)) {
            fprintf(stderr, "%s[agent] Connection to server lost.%s\n", Color::RED, Color::RESET);
            return "";
        }

        std::string response;
        bool stream_ended = false;

        while (!stream_ended) {
            nlohmann::json stream_msg;
            if (!sock_->read_json(stream_msg)) {
                fprintf(stderr, "%s\n[agent] Error reading from server%s\n", Color::RED, Color::RESET);
                break;
            }

            std::string msg_type = stream_msg.value("type", "");
            if (msg_type == "token") {
                std::string piece = stream_msg.value("piece", "");
                if (token_cb) {
                    if (!token_cb(piece)) {
                        break;
                    }
                }
                response += piece;
            } else if (msg_type == "done") {
                if (response.empty()) {
                    response = stream_msg.value("response", "");
                    if (token_cb && !response.empty()) {
                        token_cb(response);
                    }
                }
                n_ctx_ = stream_msg.value("n_ctx", n_ctx_);
                used_ctx_ = stream_msg.value("used_ctx", used_ctx_);
                stream_ended = true;
            } else if (msg_type == "error") {
                std::string err_msg = stream_msg.value("message", "unknown server error");
                fprintf(stderr, "%s\n[server error] %s%s\n", Color::RED, err_msg.c_str(), Color::RESET);
                stream_ended = true;
            }
        }
        return response;
    }

    std::string generate(std::string_view prompt,
                         AgentTokenCallback token_cb = nullptr,
                         float temp_override = -1.0f) override {
        if (!sock_ || !sock_->is_valid()) return "";

        nlohmann::json gen_req = {
            {"type", "generate"},
            {"prompt", std::string(prompt)},
            {"temperature", temp_override}
        };

        std::string summary;
        if (sock_->send_json(gen_req)) {
            bool stream_ended = false;
            while (!stream_ended) {
                nlohmann::json msg;
                if (!sock_->read_json(msg)) break;
                std::string t = msg.value("type", "");
                if (t == "token") {
                    std::string piece = msg.value("piece", "");
                    if (token_cb) {
                        token_cb(piece);
                    }
                } else if (t == "done") {
                    summary = msg.value("response", "");
                    n_ctx_ = msg.value("n_ctx", n_ctx_);
                    used_ctx_ = msg.value("used_ctx", used_ctx_);
                    stream_ended = true;
                } else if (t == "error") {
                    stream_ended = true;
                }
            }
        }
        return summary;
    }

    int get_context_size() override { return n_ctx_; }
    int get_used_context() override { return used_ctx_; }

    void refresh_context_info() override {
        if (!sock_ || !sock_->is_valid()) return;
        sock_->send_json({{"type", "context"}});
        nlohmann::json ctx_resp;
        if (sock_->read_json(ctx_resp) && ctx_resp.value("type", "") == "context") {
            n_ctx_ = ctx_resp.value("n_ctx", n_ctx_);
            used_ctx_ = ctx_resp.value("used_ctx", used_ctx_);
        }
    }

    void reset_context() override {
        if (!sock_ || !sock_->is_valid()) return;
        sock_->send_json({{"type", "reset"}});
        nlohmann::json reset_resp;
        sock_->read_json(reset_resp);
        used_ctx_ = 0;
    }

    std::string get_model_name() const override { return model_name_; }

private:
    std::unique_ptr<TcpSocket> sock_;
    std::string model_name_;
    int n_ctx_ = 0;
    int used_ctx_ = 0;
};

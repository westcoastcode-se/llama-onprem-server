#pragma once

#include <string>
#include <span>
#include <vector>
#include <nlohmann/json.hpp>

namespace Protocol {

struct ChatMessage {
    std::string role;
    std::string content;

    nlohmann::json to_json() const {
        return {{"role", role}, {"content", content}};
    }

    static ChatMessage from_json(const nlohmann::json & j) {
        ChatMessage msg;
        msg.role = j.value("role", "");
        msg.content = j.value("content", "");
        return msg;
    }
};

inline std::vector<ChatMessage> parse_messages(const nlohmann::json & j_arr) {
    std::vector<ChatMessage> msgs;
    if (j_arr.is_array()) {
        msgs.reserve(j_arr.size());
        for (const auto & item : j_arr) {
            msgs.push_back(ChatMessage::from_json(item));
        }
    }
    return msgs;
}

inline nlohmann::json messages_to_json(std::span<const ChatMessage> msgs) {
    nlohmann::json arr = nlohmann::json::array();
    for (const auto & m : msgs) {
        arr.push_back(m.to_json());
    }
    return arr;
}

} // namespace Protocol

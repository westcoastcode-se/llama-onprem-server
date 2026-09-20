#pragma once

#include "../../common/std.hpp"
#include "api/models.hpp"
#include "llama.h"

/**
 * Struct helping the server build the message sent into the LLM
 */
struct Template
{
    struct AppliedTemplate
    {
        // Template - must be null-terminated
        string_view tmpl{};

        // The formatted message sent to the LLM
        vector<char> message{};

        /**
         * Format the actual message
         *
         * @param messages
         * @param add_assistant
         * @param append
         */
        const vector<char>& format(span<const ChatMessage> messages, bool add_assistant, bool append = true)
        {
            std::vector<llama_chat_message> raw_msgs;
            uint32_t estimated_length = 0;
            raw_msgs.reserve(messages.size());
            for (const auto &m : messages)
            {
                raw_msgs.push_back({m.role.c_str(), m.content.c_str()});
                estimated_length += m.content.size() + m.role.size() + 32;
            }

            // Append the already generated message with the new templated content
            uintptr_t offset = 0;
            if (append)
            {
                offset = message.size();
                message.resize(message.size() + estimated_length);
            }

            // Prepare an initial size
            if (message.size() == 0)
            {
                message.resize(estimated_length);
            }

            // Try to apply the template. If the capacity of
            // the buffer is to small then resize it and try again
            auto len =
                llama_chat_apply_template(tmpl.c_str(), raw_msgs.data(), raw_msgs.size(), add_assistant,
                                          message.data() + offset, static_cast<int32_t>(message.size() - offset));
            if (len > static_cast<int>(message.size()))
            {
                const auto needed_length = len + offset + 1;
                message.resize(needed_length);
                len = llama_chat_apply_template(tmpl.c_str(), raw_msgs.data(), raw_msgs.size(), add_assistant,
                                                message.data(), static_cast<int32_t>(message.size()));
            }

            message.resize(len + 1);
            message[len] = 0;
            return message;
        }
    };

    // The actual template content
    string content;

    /**
     * @param messages The total number of messages
     * @param add_assistant Add assistant marker
     * @return The applied template
     */
    AppliedTemplate create() const
    {
        return {.tmpl = content, .message = {}};
    }
};
#include "responses.hpp"

#include "api/errors.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <format>
#include <optional>

namespace
{

std::string next_id(std::string_view prefix)
{
    static std::atomic<uint64_t> seq{1};
    return std::format("{}{}", prefix, seq.fetch_add(1, std::memory_order_relaxed));
}

std::string arguments_text(const nlohmann::json &arguments)
{
    if (arguments.is_string())
    {
        return arguments.get<std::string>();
    }
    if (arguments.is_null() || arguments.empty())
    {
        return "{}";
    }
    return arguments.dump();
}

std::string response_call_id(const ParsedToolCall &call, const size_t index)
{
    if (call.id.empty())
    {
        return std::format("call_{}", index + 1);
    }
    if (call.id.starts_with("call_"))
    {
        return call.id;
    }
    return "call_" + call.id;
}

std::string text_from_parts(const nlohmann::json &content)
{
    if (content.is_string())
    {
        return content.get<std::string>();
    }
    if (content.is_null())
    {
        return {};
    }
    if (!content.is_array())
    {
        throw BadRequest{"content must be a string or an array"};
    }
    std::string text;
    for (const auto &part : content)
    {
        if (part.is_string())
        {
            text += part.get<std::string>();
            continue;
        }
        if (!part.is_object())
        {
            continue;
        }
        const auto type = part.value("type", "");
        if (type == "input_image" || type == "input_audio" || type == "image_url" || type == "input_file")
        {
            throw BadRequest{"image, audio, and file input are not supported"};
        }
        const char *key = part.contains("text") ? "text" : part.contains("refusal") ? "refusal" : nullptr;
        if (key == nullptr || !part.at(key).is_string())
        {
            continue;
        }
        if (!text.empty())
        {
            text.push_back('\n');
        }
        text += part.at(key).get<std::string>();
    }
    return text;
}

std::string item_text(const nlohmann::json &item)
{
    if (!item.contains("content") || item.at("content").is_null())
    {
        return {};
    }
    return text_from_parts(item.at("content"));
}

std::string reasoning_text(const nlohmann::json &item)
{
    std::string text;
    const auto take = [&](const char *key) {
        if (!item.contains(key) || item.at(key).is_null() || !item.at(key).is_array())
        {
            return;
        }
        for (const auto &part : item.at(key))
        {
            if (!part.is_object() || !part.contains("text") || !part.at("text").is_string())
            {
                continue;
            }
            if (!text.empty())
            {
                text.push_back('\n');
            }
            text += part.at("text").get<std::string>();
        }
    };
    take("content");
    if (text.empty())
    {
        take("summary");
    }
    return text;
}

void merge_assistant(std::vector<ChatMessage> &messages, ChatMessage incoming)
{
    if (!messages.empty() && messages.back().role == ChatMessage::ROLE_ASSISTANT)
    {
        auto &prev = messages.back();
        if (!incoming.content.empty())
        {
            if (!prev.content.empty())
            {
                prev.content.push_back('\n');
            }
            prev.content += incoming.content;
        }
        if (!incoming.reasoning_content.empty())
        {
            if (!prev.reasoning_content.empty())
            {
                prev.reasoning_content.push_back('\n');
            }
            prev.reasoning_content += incoming.reasoning_content;
        }
        prev.tool_calls.insert(prev.tool_calls.end(), std::make_move_iterator(incoming.tool_calls.begin()),
                               std::make_move_iterator(incoming.tool_calls.end()));
        return;
    }
    messages.push_back(std::move(incoming));
}

ParsedToolCall tool_call_from_item(const nlohmann::json &item, const bool custom)
{
    ParsedToolCall call;
    call.name = item.value("name", "");
    if (call.name.empty())
    {
        throw BadRequest{"function_call requires name"};
    }
    call.id = item.value("call_id", "");
    const char *key = custom ? "input" : "arguments";
    if (!item.contains(key) || item.at(key).is_null())
    {
        call.arguments = nlohmann::json::object();
        return call;
    }
    const auto &args = item.at(key);
    if (args.is_string())
    {
        const auto text = args.get<std::string>();
        if (text.empty())
        {
            call.arguments = nlohmann::json::object();
            return call;
        }
        try
        {
            call.arguments = nlohmann::json::parse(text);
        }
        catch (const nlohmann::json::exception &)
        {
            call.arguments = text;
        }
        return call;
    }
    call.arguments = args;
    return call;
}

void append_tool_output(std::vector<ChatMessage> &messages, const nlohmann::json &item)
{
    if (!item.contains("output"))
    {
        throw BadRequest{"function_call_output requires output"};
    }
    ChatMessage message;
    message.role = std::string(ChatMessage::ROLE_TOOL);
    message.tool_call_id = item.value("call_id", "");
    message.tool_name = item.value("name", "");
    message.content = text_from_parts(item.at("output"));
    messages.push_back(std::move(message));
}

std::string cache_session_id(std::string_view key)
{
    std::string out;
    out.reserve(std::min(key.size(), size_t{120}));
    for (const unsigned char ch : key)
    {
        if (out.size() == 120)
        {
            break;
        }
        const bool ok = (ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || ch == '.' ||
                        ch == '_' || ch == '-';
        if (ok)
        {
            out.push_back(static_cast<char>(ch));
        }
        else
        {
            out.push_back('_');
        }
    }
    if (out.empty())
    {
        return "codex";
    }
    return out;
}

struct Marker
{
    std::string_view start;
    std::string_view end;
    bool reasoning = false;
};

// Longest match wins when one marker is a prefix of another (<tool_call> / <tool_calls>).
constexpr Marker kMarkers[] = {
    {"<think>", "</think>", true},
    {"<tool_calls>", "</tool_calls>", false},
    {"<tool_call>", "</tool_call>", false},
    {"<｜tool▁calls▁begin｜>", "<｜tool▁calls▁end｜>", false},
    {"<｜tool▁call▁begin｜>", "<｜tool▁call▁end｜>", false},
    {"[TOOL_CALLS]", "", false},
};

size_t hold_suffix(std::string_view text, const auto &markers)
{
    size_t hold = 0;
    for (const std::string_view marker : markers)
    {
        if (marker.size() < 2)
        {
            continue;
        }
        const size_t max = std::min(text.size(), marker.size() - 1);
        for (size_t length = max; length > hold; --length)
        {
            if (text.substr(text.size() - length) == marker.substr(0, length))
            {
                hold = length;
                break;
            }
        }
    }
    return hold;
}

struct Hit
{
    size_t pos = 0;
    const Marker *marker = nullptr;
};

std::optional<Hit> earliest_marker(std::string_view text)
{
    std::optional<Hit> best;
    for (const auto &marker : kMarkers)
    {
        const auto pos = text.find(marker.start);
        if (pos == std::string_view::npos)
        {
            continue;
        }
        if (!best || pos < best->pos || (pos == best->pos && marker.start.size() > best->marker->start.size()))
        {
            best = Hit{pos, &marker};
        }
    }
    return best;
}

} // namespace

ResponsesTurn responses_from_json(const nlohmann::json &body)
{
    if (!body.is_object())
    {
        throw BadRequest{"request body must be a JSON object"};
    }
    if (!body.contains("input"))
    {
        throw BadRequest{"input is required"};
    }

    ResponsesTurn turn;
    turn.model = body.value("model", "");
    turn.instructions = body.value("instructions", "");
    turn.stream = body.value("stream", false);
    if (body.contains("max_output_tokens") && body.at("max_output_tokens").is_number_integer())
    {
        turn.max_tokens = body.at("max_output_tokens").get<int>();
    }
    if (body.contains("temperature") && body.at("temperature").is_number())
    {
        turn.temperature = body.at("temperature").get<float>();
    }
    if (body.contains("prompt_cache_key") && body.at("prompt_cache_key").is_string())
    {
        turn.session_id = cache_session_id(body.at("prompt_cache_key").get<std::string>());
    }
    else
    {
        turn.session_id = "codex";
    }

    if (body.contains("tools"))
    {
        if (!body.at("tools").is_array())
        {
            throw BadRequest{"tools must be an array"};
        }
        for (const auto &tool : body.at("tools"))
        {
            if (!tool.is_object())
            {
                throw BadRequest{"each tool must be an object"};
            }
            const auto type = tool.value("type", "function");
            if (type != "function")
            {
                continue;
            }
            turn.tools.push_back(ChatTool::from_json(tool));
        }
    }

    const auto &input = body.at("input");
    if (input.is_string())
    {
        ChatMessage message;
        message.role = std::string(ChatMessage::ROLE_USER);
        message.content = input.get<std::string>();
        turn.messages.push_back(std::move(message));
        return turn;
    }
    if (!input.is_array())
    {
        throw BadRequest{"input must be a string or an array"};
    }

    for (const auto &item : input)
    {
        if (!item.is_object())
        {
            throw BadRequest{"each input item must be an object"};
        }
        const auto type = item.value("type", "");
        if (type == "function_call" || type == "custom_tool_call")
        {
            ChatMessage message;
            message.role = std::string(ChatMessage::ROLE_ASSISTANT);
            message.tool_calls.push_back(tool_call_from_item(item, type == "custom_tool_call"));
            merge_assistant(turn.messages, std::move(message));
            continue;
        }
        if (type == "function_call_output" || type == "custom_tool_call_output")
        {
            append_tool_output(turn.messages, item);
            continue;
        }
        if (type == "reasoning")
        {
            const auto text = reasoning_text(item);
            if (text.empty())
            {
                continue;
            }
            ChatMessage message;
            message.role = std::string(ChatMessage::ROLE_ASSISTANT);
            message.reasoning_content = text;
            merge_assistant(turn.messages, std::move(message));
            continue;
        }
        if (type.empty() || type == "message")
        {
            if (!item.contains("role") || !item.at("role").is_string())
            {
                continue;
            }
            auto role = item.at("role").get<std::string>();
            if (role == "developer")
            {
                role = std::string(ChatMessage::ROLE_SYSTEM);
            }
            ChatMessage message;
            message.role = std::move(role);
            message.content = item_text(item);
            if (message.role == ChatMessage::ROLE_ASSISTANT)
            {
                merge_assistant(turn.messages, std::move(message));
            }
            else
            {
                turn.messages.push_back(std::move(message));
            }
            continue;
        }
        // Compaction, web_search_call, and other Codex bookkeeping stay out of the prompt.
    }
    return turn;
}

std::string public_model_id(const std::string_view model_path)
{
    const auto slash = model_path.find_last_of("/\\");
    std::string name{slash == std::string_view::npos ? model_path : model_path.substr(slash + 1)};
    constexpr std::string_view kSuffix = ".gguf";
    if (name.size() > kSuffix.size() && name.ends_with(kSuffix))
    {
        name.resize(name.size() - kSuffix.size());
    }
    if (name.empty())
    {
        return "local";
    }
    return name;
}

ResponseIds ResponseIds::make()
{
    return ResponseIds{next_id("resp_"), next_id("rs_"), next_id("msg_")};
}

std::vector<nlohmann::json> response_output_items(const ResponseIds &ids, const ResponsesResult &result)
{
    std::vector<nlohmann::json> output;
    if (!result.reasoning.empty())
    {
        output.push_back({
            {"id", ids.reasoning},
            {"type", "reasoning"},
            {"status", "completed"},
            {"summary", nlohmann::json::array()},
            {"encrypted_content", ""},
            {"content", nlohmann::json::array({{{"type", "reasoning_text"}, {"text", result.reasoning}}})},
        });
    }
    if (!result.text.empty() || result.tool_calls.empty())
    {
        output.push_back({
            {"id", ids.message},
            {"type", "message"},
            {"status", "completed"},
            {"role", "assistant"},
            {"content", nlohmann::json::array({{{"type", "output_text"}, {"text", result.text}}})},
        });
    }
    for (size_t i = 0; i < result.tool_calls.size(); ++i)
    {
        const auto &call = result.tool_calls[i];
        const auto call_id = response_call_id(call, i);
        output.push_back({
            {"id", "fc_" + call_id},
            {"type", "function_call"},
            {"status", "completed"},
            {"name", call.name},
            {"arguments", arguments_text(call.arguments)},
            {"call_id", call_id},
        });
    }
    return output;
}

std::vector<nlohmann::json> response_terminal_events(const ResponseIds &ids, const std::string_view model,
                                                     const ResponsesResult &result)
{
    std::vector<nlohmann::json> events;
    if (result.status == ResponsesResult::Status::Failed)
    {
        events.push_back({
            {"type", "response.failed"},
            {"response",
             {{"id", ids.response},
              {"object", "response"},
              {"status", "failed"},
              {"model", model},
              {"error", {{"message", result.error}, {"code", result.error_code}}}}},
        });
        return events;
    }
    if (result.status == ResponsesResult::Status::Cancelled)
    {
        events.push_back({
            {"type", "response.incomplete"},
            {"response",
             {{"id", ids.response},
              {"object", "response"},
              {"status", "incomplete"},
              {"model", model},
              {"incomplete_details", {{"reason", "interrupted"}}}}},
        });
        return events;
    }

    const auto output = response_output_items(ids, result);
    for (const auto &item : output)
    {
        events.push_back({{"type", "response.output_item.done"}, {"item", item}});
    }
    events.push_back({
        {"type", "response.completed"},
        {"response",
         {{"id", ids.response},
          {"object", "response"},
          {"status", "completed"},
          {"model", model},
          {"output", output}}},
    });
    return events;
}

std::string sse_frame(const nlohmann::json &data)
{
    return std::format("event: {}\ndata: {}\n\n", data.value("type", ""), data.dump());
}

std::string responses_visible_text(const std::string_view raw)
{
    ResponsesDeltaFilter filter;
    auto head = filter.feed(raw);
    const auto tail = filter.finish();
    head.text += tail.text;
    return head.text;
}

ResponsesDeltaFilter::Delta ResponsesDeltaFilter::feed(const std::string_view piece)
{
    pending_.append(piece);
    return drain(false);
}

ResponsesDeltaFilter::Delta ResponsesDeltaFilter::finish()
{
    return drain(true);
}

ResponsesDeltaFilter::Delta ResponsesDeltaFilter::drain(const bool flush)
{
    Delta out;
    for (;;)
    {
        if (mode_ == Mode::Text)
        {
            if (const auto hit = earliest_marker(pending_))
            {
                out.text.append(pending_, 0, hit->pos);
                pending_.erase(0, hit->pos + hit->marker->start.size());
                if (hit->marker->reasoning)
                {
                    mode_ = Mode::Reasoning;
                }
                else
                {
                    mode_ = Mode::Tool;
                    tool_end_ = std::string(hit->marker->end);
                }
                continue;
            }
            const size_t hold = flush ? 0 : hold_suffix(pending_, std::array{kMarkers[0].start, kMarkers[1].start, kMarkers[2].start,
                                                                              kMarkers[3].start, kMarkers[4].start, kMarkers[5].start});
            out.text.append(pending_, 0, pending_.size() - hold);
            pending_.erase(0, pending_.size() - hold);
            break;
        }
        if (mode_ == Mode::Reasoning)
        {
            constexpr std::string_view kEnd = "</think>";
            const auto end = pending_.find(kEnd);
            if (end != std::string::npos)
            {
                out.reasoning.append(pending_, 0, end);
                pending_.erase(0, end + kEnd.size());
                mode_ = Mode::Text;
                continue;
            }
            const size_t hold = flush ? 0 : hold_suffix(pending_, std::array{kEnd});
            out.reasoning.append(pending_, 0, pending_.size() - hold);
            pending_.erase(0, pending_.size() - hold);
            if (flush)
            {
                mode_ = Mode::Text;
            }
            break;
        }

        if (tool_end_.empty())
        {
            pending_.clear();
            if (flush)
            {
                mode_ = Mode::Text;
            }
            break;
        }
        const auto end = pending_.find(tool_end_);
        if (end != std::string::npos)
        {
            pending_.erase(0, end + tool_end_.size());
            tool_end_.clear();
            mode_ = Mode::Text;
            continue;
        }
        const size_t hold = flush ? 0 : hold_suffix(pending_, std::array{std::string_view{tool_end_}});
        pending_.erase(0, pending_.size() - hold);
        if (flush)
        {
            pending_.clear();
            tool_end_.clear();
            mode_ = Mode::Text;
        }
        break;
    }
    return out;
}

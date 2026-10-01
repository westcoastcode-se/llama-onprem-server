#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

// Client-side reading of a model reply. The live agent uses the server's
// parse_assistant_actions instead. These types stay for the tag parser tests.
class ResponseBlocks
{
  public:
    struct ToolCall
    {
        std::string_view value{};
        bool is_done{};
    };

    std::string_view thinking{};
    std::vector<ToolCall> tool_calls{};
    std::string_view questions{};
    std::vector<std::string_view> answers{};

    static constexpr int thinking_bit = 1 << 0;
    static constexpr int thinking_done_bit = 1 << 1;
    static constexpr int done_bit = 1 << 4;

    int flags = 0;

    [[nodiscard]] bool is_done() const
    {
        return flags & done_bit;
    }

    static std::string_view::size_type extract_string(std::string_view &thinking, std::string_view text, std::string_view tag,
                                                       std::size_t pos);

    static ResponseBlocks from_text(std::string_view text);
};

class ThinkingStreamFilter
{
  public:
    using OutputCallback = std::function<void(std::string_view piece, bool is_thinking)>;

    explicit ThinkingStreamFilter(OutputCallback cb);

    void process(std::string_view piece);
    void flush();

  private:
    enum class State
    {
        NORMAL,
        BUFFERING_THINKING,
        STREAMING_THINKING
    };

    OutputCallback cb_;
    State state_ = State::NORMAL;
    std::string buffer_;

    void process_internal();
    void emit_normal(std::string_view piece);
    void emit_thinking(std::string_view piece);
};

struct ToolCall
{
    std::string name;
    nlohmann::json arguments;
};

std::string strip_think_tags(std::string_view text);

bool parse_tool_calls(std::string_view response, std::vector<ToolCall> &tool_calls, std::string *out_error = nullptr);

bool parse_tool_call(std::string_view response, std::string &name, nlohmann::json &arguments, std::string *out_error = nullptr);

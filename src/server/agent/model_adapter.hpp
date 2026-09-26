#pragma once

#include "../../api/models.hpp"

#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// How one model family writes tool calls, and the instructions that ask it to do so.
// Question and answer tags stay in the shared assistant parser.
class ModelAdapter
{
  public:
    virtual ~ModelAdapter() = default;

    [[nodiscard]] virtual std::string_view name() const = 0;

    // One example call in this model's syntax, using the client's tools. Empty when tools is empty.
    [[nodiscard]] virtual std::string example_call(std::span<const ChatTool> tools) const = 0;

    // Append tool calls found in text. Does not assign ids.
    virtual void parse_tool_calls(std::string_view text, std::vector<ParsedToolCall> &out) const = 0;
};

// One property from a tool's JSON schema. Model-neutral.
struct ToolParameter
{
    std::string name;
    std::string type;
    std::string description;
    bool required = false;
};

[[nodiscard]] std::vector<ToolParameter> tool_parameters(const ChatTool &tool);

// XML parameters are stored as raw text. This turns integer/boolean/array/object
// fields into JSON when the tool schema says so, and turns a non-string back into
// text when the schema says string. Unknown tools are left as parsed.
void coerce_tool_arguments(std::vector<ParsedToolCall> &calls, std::span<const ChatTool> tools);

// <tool_call><function=name><parameter=key>value</parameter></function></tool_call>
class QwenAdapter final : public ModelAdapter
{
  public:
    [[nodiscard]] std::string_view name() const override;
    [[nodiscard]] std::string example_call(std::span<const ChatTool> tools) const override;
    void parse_tool_calls(std::string_view text, std::vector<ParsedToolCall> &out) const override;
};

// <｜tool▁call▁begin｜>function<｜tool▁sep｜>name```json {...}```<｜tool▁call▁end｜>
class DeepseekAdapter final : public ModelAdapter
{
  public:
    [[nodiscard]] std::string_view name() const override;
    [[nodiscard]] std::string example_call(std::span<const ChatTool> tools) const override;
    void parse_tool_calls(std::string_view text, std::vector<ParsedToolCall> &out) const override;
};

// The shipped Ternary Bonsai template uses the same call syntax as Qwen.
// The class is separate so its prompt and parser can diverge without touching Qwen.
class BonsaiAdapter final : public ModelAdapter
{
  public:
    [[nodiscard]] std::string_view name() const override;
    [[nodiscard]] std::string example_call(std::span<const ChatTool> tools) const override;
    void parse_tool_calls(std::string_view text, std::vector<ParsedToolCall> &out) const override;
};

// Template name wins over the model path when it names a known family. Otherwise the model path is used.
// Unknown names use Qwen, which is the format this server was built against.
[[nodiscard]] std::unique_ptr<ModelAdapter> make_model_adapter(std::string_view model_path,
                                                              std::string_view template_path);

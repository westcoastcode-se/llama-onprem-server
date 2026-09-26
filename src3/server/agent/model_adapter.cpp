#include "model_adapter.hpp"

#include <algorithm>
#include <cctype>
#include <format>
#include <utility>

namespace
{

std::string_view trim_sv(std::string_view text)
{
    constexpr std::string_view ws = " \t\r\n";
    const auto begin = text.find_first_not_of(ws);
    if (begin == std::string_view::npos)
    {
        return {};
    }
    const auto end = text.find_last_not_of(ws);
    return text.substr(begin, end - begin + 1);
}

std::string lower_copy(std::string_view text)
{
    std::string out(text);
    for (char &ch : out)
    {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return out;
}

bool contains(std::string_view haystack, std::string_view needle)
{
    return haystack.find(needle) != std::string_view::npos;
}

void append_function_parameter_calls(std::string_view text, std::vector<ParsedToolCall> &out)
{
    constexpr std::string_view kFunction = "<function=";
    constexpr std::string_view kFunctionEnd = "</function>";
    constexpr std::string_view kParameter = "<parameter=";
    constexpr std::string_view kParameterEnd = "</parameter>";

    size_t pos = 0;
    while (pos < text.size())
    {
        const size_t start = text.find(kFunction, pos);
        if (start == std::string_view::npos)
        {
            break;
        }
        const size_t name_begin = start + kFunction.size();
        const size_t name_end = text.find('>', name_begin);
        if (name_end == std::string_view::npos)
        {
            break;
        }
        const size_t end = text.find(kFunctionEnd, name_end);
        if (end == std::string_view::npos)
        {
            break;
        }

        ParsedToolCall call;
        call.name = std::string(trim_sv(text.substr(name_begin, name_end - name_begin)));
        const std::string_view body = text.substr(name_end + 1, end - (name_end + 1));
        call.arguments = nlohmann::json::object();

        size_t param = 0;
        while (param < body.size())
        {
            const size_t ps = body.find(kParameter, param);
            if (ps == std::string_view::npos)
            {
                break;
            }
            const size_t nb = ps + kParameter.size();
            const size_t ne = body.find('>', nb);
            if (ne == std::string_view::npos)
            {
                break;
            }
            const size_t pe = body.find(kParameterEnd, ne);
            if (pe == std::string_view::npos)
            {
                break;
            }
            const auto pname = std::string(trim_sv(body.substr(nb, ne - nb)));
            const auto value = trim_sv(body.substr(ne + 1, pe - (ne + 1)));
            if (!pname.empty())
            {
                try
                {
                    call.arguments[pname] = nlohmann::json::parse(value);
                }
                catch (...)
                {
                    call.arguments[pname] = std::string(value);
                }
            }
            param = pe + kParameterEnd.size();
        }
        if (!call.name.empty())
        {
            out.push_back(std::move(call));
        }
        pos = end + kFunctionEnd.size();
    }
}

std::string_view strip_json_fence(std::string_view text)
{
    text = trim_sv(text);
    if (text.starts_with("```"))
    {
        const auto nl = text.find('\n');
        if (nl != std::string_view::npos)
        {
            text.remove_prefix(nl + 1);
        }
        if (text.ends_with("```"))
        {
            text.remove_suffix(3);
        }
        text = trim_sv(text);
    }
    return text;
}

nlohmann::json parse_argument_object(std::string_view payload)
{
    auto parsed = nlohmann::json::parse(strip_json_fence(payload));
    if (parsed.is_string())
    {
        parsed = nlohmann::json::parse(parsed.get<std::string>());
    }
    if (!parsed.is_object())
    {
        return nlohmann::json::object();
    }
    return parsed;
}

std::string parameter_placeholder(const ToolParameter &parameter)
{
    if (parameter.type == "integer" || parameter.type == "number")
    {
        return "0";
    }
    if (parameter.type == "boolean")
    {
        return "false";
    }
    if (!parameter.description.empty())
    {
        return parameter.description;
    }
    return parameter.name;
}

std::string function_parameter_example(std::span<const ChatTool> tools)
{
    if (tools.empty())
    {
        return {};
    }
    const ChatTool &tool = tools.front();
    std::string example = "Call a tool in this format:\n<tool_call>\n<function=";
    example += tool.name;
    example += ">\n";
    for (const ToolParameter &parameter : tool_parameters(tool))
    {
        example += "<parameter=";
        example += parameter.name;
        example += ">\n";
        example += parameter_placeholder(parameter);
        example += "\n</parameter>\n";
    }
    example += "</function>\n</tool_call>\n\n"
               "Put each argument in its own <parameter=name> block. The text inside the block is the value, "
               "not a JSON object.\n\n";
    return example;
}

} // namespace

std::vector<ToolParameter> tool_parameters(const ChatTool &tool)
{
    std::vector<ToolParameter> parameters;
    if (tool.parameters.empty())
    {
        return parameters;
    }
    const nlohmann::json schema = nlohmann::json::parse(tool.parameters, nullptr, false);
    if (schema.is_discarded() || !schema.is_object())
    {
        return parameters;
    }
    const auto properties = schema.find("properties");
    if (properties == schema.end() || !properties->is_object())
    {
        return parameters;
    }
    const nlohmann::json required = schema.value("required", nlohmann::json::array());
    for (auto it = properties->begin(); it != properties->end(); ++it)
    {
        ToolParameter parameter;
        parameter.name = it.key();
        if (it->is_object())
        {
            parameter.type = it->value("type", "string");
            parameter.description = it->value("description", "");
        }
        if (required.is_array())
        {
            for (const auto &item : required)
            {
                if (item.is_string() && item.get<std::string>() == parameter.name)
                {
                    parameter.required = true;
                    break;
                }
            }
        }
        parameters.push_back(std::move(parameter));
    }
    return parameters;
}

std::string_view QwenAdapter::name() const
{
    return "qwen";
}

std::string QwenAdapter::example_call(std::span<const ChatTool> tools) const
{
    return function_parameter_example(tools);
}

void QwenAdapter::parse_tool_calls(std::string_view text, std::vector<ParsedToolCall> &out) const
{
    append_function_parameter_calls(text, out);
}

std::string_view DeepseekAdapter::name() const
{
    return "deepseek";
}

std::string DeepseekAdapter::example_call(std::span<const ChatTool> tools) const
{
    if (tools.empty())
    {
        return {};
    }
    const ChatTool &tool = tools.front();
    nlohmann::json arguments = nlohmann::json::object();
    for (const ToolParameter &parameter : tool_parameters(tool))
    {
        const std::string placeholder = parameter_placeholder(parameter);
        if (parameter.type == "integer")
        {
            arguments[parameter.name] = 0;
        }
        else if (parameter.type == "number")
        {
            arguments[parameter.name] = 0.0;
        }
        else if (parameter.type == "boolean")
        {
            arguments[parameter.name] = false;
        }
        else
        {
            arguments[parameter.name] = placeholder;
        }
    }
    return std::format("Call a tool in this format:\n"
                       "<｜tool▁calls▁begin｜><｜tool▁call▁begin｜>function<｜tool▁sep｜>{}\n"
                       "```json\n"
                       "{}\n"
                       "```\n"
                       "<｜tool▁call▁end｜><｜tool▁calls▁end｜>\n\n"
                       "The text after <｜tool▁sep｜> is the function name. The fenced block is a JSON object of "
                       "arguments.\n\n",
                       tool.name, arguments.dump(2));
}

void DeepseekAdapter::parse_tool_calls(std::string_view text, std::vector<ParsedToolCall> &out) const
{
    constexpr std::string_view kBegin = "<｜tool▁call▁begin｜>";
    constexpr std::string_view kSep = "<｜tool▁sep｜>";
    constexpr std::string_view kEnd = "<｜tool▁call▁end｜>";

    size_t pos = 0;
    while (pos < text.size())
    {
        const size_t start = text.find(kBegin, pos);
        if (start == std::string_view::npos)
        {
            break;
        }
        const size_t end = text.find(kEnd, start + kBegin.size());
        if (end == std::string_view::npos)
        {
            break;
        }
        const std::string_view body = text.substr(start + kBegin.size(), end - (start + kBegin.size()));
        const size_t sep = body.find(kSep);
        if (sep == std::string_view::npos)
        {
            pos = end + kEnd.size();
            continue;
        }

        const auto left = trim_sv(body.substr(0, sep));
        const auto right = body.substr(sep + kSep.size());
        std::string name;
        std::string_view args;
        if (left == "function")
        {
            const size_t nl = right.find('\n');
            name = std::string(trim_sv(nl == std::string_view::npos ? right : right.substr(0, nl)));
            args = nl == std::string_view::npos ? std::string_view{} : right.substr(nl + 1);
        }
        else
        {
            name = std::string(left);
            args = right;
        }

        ParsedToolCall call;
        call.name = std::move(name);
        if (!call.name.empty())
        {
            try
            {
                call.arguments = parse_argument_object(args);
            }
            catch (...)
            {
                call.arguments = nlohmann::json::object();
            }
            out.push_back(std::move(call));
        }
        pos = end + kEnd.size();
    }
}

std::string_view BonsaiAdapter::name() const
{
    return "bonsai";
}

std::string BonsaiAdapter::example_call(std::span<const ChatTool> tools) const
{
    return function_parameter_example(tools);
}

void BonsaiAdapter::parse_tool_calls(std::string_view text, std::vector<ParsedToolCall> &out) const
{
    append_function_parameter_calls(text, out);
}

std::unique_ptr<ModelAdapter> make_model_adapter(std::string_view model_path, std::string_view template_path)
{
    const auto pick = [](std::string_view hint) -> std::unique_ptr<ModelAdapter> {
        const std::string text = lower_copy(hint);
        if (contains(text, "deepseek"))
        {
            return std::make_unique<DeepseekAdapter>();
        }
        if (contains(text, "bonsai") || contains(text, "ternary"))
        {
            return std::make_unique<BonsaiAdapter>();
        }
        if (contains(text, "qwen"))
        {
            return std::make_unique<QwenAdapter>();
        }
        return nullptr;
    };

    if (auto from_template = pick(template_path))
    {
        return from_template;
    }
    if (auto from_model = pick(model_path))
    {
        return from_model;
    }
    return std::make_unique<QwenAdapter>();
}

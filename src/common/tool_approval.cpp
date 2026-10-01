#include "common/tools.hpp"
#include "common/color.hpp"

#include <algorithm>
#include <cctype>
#include <iostream>
#include <string>

ToolApprovalParseResult parse_tool_approval_input(std::string_view raw_input) {
    size_t first = raw_input.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) {
        return ToolApprovalParseResult::INVALID;
    }
    size_t last = raw_input.find_last_not_of(" \t\r\n");
    std::string input(raw_input.substr(first, last - first + 1));

    std::transform(input.begin(), input.end(), input.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });

    if (input == "ja" || input == "j" || input == "yes" || input == "y" || input == "1") {
        return ToolApprovalParseResult::ALLOW;
    }
    if (input == "nej" || input == "n" || input == "no" || input == "0") {
        return ToolApprovalParseResult::DENY;
    }
    if (input == "alltid ja" || input == "alltid" || input == "alltidja" ||
        input == "always ja" || input == "always yes" || input == "always" ||
        input == "a" || input == "all") {
        return ToolApprovalParseResult::ALWAYS;
    }

    return ToolApprovalParseResult::INVALID;
}

ToolApproval prompt_tool_approval(std::string_view tool_name, const nlohmann::json &tool_args, std::istream &in,
                                  std::ostream &out) {
    while (true) {
        out << Color::BOLD << Color::YELLOW << "⚠️  Agent vill köra verktyg / wants to execute tool: "
                << Color::CYAN << tool_name << Color::RESET << "\n";
        out << "   " << Color::GRAY << "Parametrar / Arguments: " << Color::RESET << tool_args.dump(2) << "\n";
        out << "   " << Color::BOLD << "Godkänn körning? (ja / nej / alltid ja) [j/n/a]: " << Color::RESET;
        out.flush();

        std::string line;
        if (!std::getline(in, line)) {
            out << "\n";
            return ToolApproval::CLOSED;
        }

        auto res = parse_tool_approval_input(line);
        switch (res) {
            case ToolApprovalParseResult::ALLOW:
                return ToolApproval::ALLOW;
            case ToolApprovalParseResult::DENY:
                return ToolApproval::DENY;
            case ToolApprovalParseResult::ALWAYS:
                return ToolApproval::ALWAYS;
            case ToolApprovalParseResult::INVALID:
                out << Color::RED << "Ogiltigt val. Ange 'j' (ja), 'n' (nej) eller 'a' (alltid ja)." << Color::RESET << "\n";
                break;
        }
    }
}

std::vector<std::string> parse_allowed_tools(std::string_view tools_str) {
    std::vector<std::string> result;
    if (tools_str.empty()) {
        return result;
    }

    size_t start = 0;
    while (start < tools_str.size()) {
        size_t end = tools_str.find(',', start);
        if (end == std::string_view::npos) {
            end = tools_str.size();
        }

        std::string_view token = tools_str.substr(start, end - start);
        size_t first = token.find_first_not_of(" \t\r\n");
        if (first != std::string_view::npos) {
            size_t last = token.find_last_not_of(" \t\r\n");
            std::string t(token.substr(first, last - first + 1));
            std::transform(t.begin(), t.end(), t.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
            result.push_back(std::move(t));
        }

        start = end + 1;
    }
    return result;
}

bool is_tool_allowed(std::string_view tool_name, bool auto_approve, std::span<const std::string> allowed_tools) {
    if (auto_approve) {
        return true;
    }
    std::string lower_tool(tool_name);
    std::transform(lower_tool.begin(), lower_tool.end(), lower_tool.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });

    for (const auto & allowed : allowed_tools) {
        if (allowed == "*" || allowed == "all" || allowed == lower_tool) {
            return true;
        }
    }
    return false;
}


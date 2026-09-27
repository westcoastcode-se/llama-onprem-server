#pragma once

#include "cli/ui.hpp"

#include "common/color.hpp"

#include <cctype>
#include <iostream>
#include <print>
#include <ranges>
#include <string>
#include <utility>

// Plain-text session used by exec. Lines go to the terminal, not the fullscreen UI.
class ConsoleUi final : public AgentUi
{
    bool thinking = false;

  public:
    void set_status(std::string status) override
    {
        std::println(stderr, "{}", status);
    }

    void set_context(int, int) override
    {
    }

    void note(std::string text) override
    {
        std::println(stderr, "{}", text);
    }

    void begin(std::string kind) override
    {
        thinking = kind == "thinking";
        if (thinking)
        {
            std::print("{}", Color::GRAY);
        }
    }

    void append(std::string text) override
    {
        std::print("{}", text);
    }

    void end() override
    {
        if (thinking)
        {
            std::println("{}", Color::RESET);
        }
        else
        {
            std::println("");
        }
        thinking = false;
    }

    Ask ask(std::string title, std::string body) override
    {
        std::println(stderr, "{}", title);
        if (!body.empty())
        {
            std::println(stderr, "{}", body);
        }
        std::string line;
        if (!std::getline(std::cin, line))
        {
            return Ask::Closed;
        }
        if (line.empty() || line == "y" || line == "yes")
        {
            return Ask::Once;
        }
        if (line == "a" || line == "always")
        {
            return Ask::AlwaysTool;
        }
        if (line == "f" || line == "full")
        {
            return Ask::Full;
        }
        return Ask::Deny;
    }

    std::optional<std::string> question(std::string prompt, std::vector<std::string> answers) override
    {
        std::println(stderr, "{}", prompt);
        for (std::size_t i = 0; i < answers.size(); ++i)
        {
            std::println(stderr, "  {} {}", i, answers[i]);
        }
        std::print(stderr, "answer> ");
        std::fflush(stderr);
        std::string line;
        if (!std::getline(std::cin, line) || line.empty())
        {
            return std::nullopt;
        }
        const bool digits = std::ranges::all_of(line, [](unsigned char c) { return std::isdigit(c) != 0; });
        if (digits && !answers.empty())
        {
            try
            {
                const int index = std::stoi(line);
                if (index >= 0 && static_cast<std::size_t>(index) < answers.size())
                {
                    return answers[static_cast<std::size_t>(index)];
                }
            }
            catch (const std::exception &)
            {
            }
        }
        return line;
    }

    std::optional<std::size_t> choose(std::string prompt, std::vector<std::string> choices, std::size_t selected) override
    {
        std::println(stderr, "{}", prompt);
        for (std::size_t i = 0; i < choices.size(); ++i)
        {
            std::println(stderr, "  {} {}", i + 1, choices[i]);
        }
        if (selected < choices.size())
        {
            std::println(stderr, "current {}", selected + 1);
        }
        std::print(stderr, "model> ");
        std::fflush(stderr);
        std::string line;
        if (!std::getline(std::cin, line) || line.empty())
        {
            return std::nullopt;
        }
        const bool digits = std::ranges::all_of(line, [](unsigned char c) { return std::isdigit(c) != 0; });
        if (!digits)
        {
            return std::nullopt;
        }
        try
        {
            const int number = std::stoi(line);
            if (number >= 1 && static_cast<std::size_t>(number) <= choices.size())
            {
                return static_cast<std::size_t>(number - 1);
            }
        }
        catch (const std::exception &)
        {
        }
        return std::nullopt;
    }

    std::string read_line() override
    {
        std::string line;
        if (!std::getline(std::cin, line))
        {
            return {};
        }
        return line;
    }
};

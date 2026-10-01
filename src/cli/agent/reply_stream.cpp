#include "cli/agent/reply_stream.hpp"

#include "cli/tui/visible_text.hpp"

#include "cli/rest_client.hpp"

#include <string>
#include <string_view>
#include <thread>
#include <utility>

void ReplyStream::read(JobKey key)
{
    RestClient &client = *state_.client;
    const SessionID session_id = state_.session;
    std::string pending;
    bool thinking = false;
    bool think_open = false;
    bool answer_open = false;
    VisibleText hidden;
    auto show_assistant = [&](std::string_view text) {
        if (!answer_open)
        {
            while (!text.empty() && (text.front() == '\n' || text.front() == '\r'))
            {
                text.remove_prefix(1);
            }
        }
        if (text.empty())
        {
            return;
        }
        if (!answer_open)
        {
            state_.ui->begin("assistant");
            answer_open = true;
        }
        state_.ui->append(std::string(text));
    };
    auto emit = [&](std::string_view text) {
        if (text.empty())
        {
            return;
        }
        if (state_.debug)
        {
            show_assistant(text);
            return;
        }
        show_assistant(hidden.feed(text));
    };
    auto close_think = [&] {
        if (think_open)
        {
            state_.ui->end();
            think_open = false;
        }
    };
    auto take_think = [&](std::string_view text) {
        if (!state_.show_think)
        {
            return;
        }
        while (!think_open && !text.empty() && (text.front() == '\n' || text.front() == '\r'))
        {
            text.remove_prefix(1);
        }
        if (text.empty())
        {
            return;
        }
        if (!think_open)
        {
            state_.ui->begin("thinking");
            think_open = true;
        }
        state_.ui->append(std::string(text));
    };

    std::jthread watcher([&](std::stop_token stop) {
        while (!stop.stop_requested())
        {
            if (g_agent_interrupt.load(std::memory_order_relaxed))
            {
                client.stop();
                try
                {
                    RestClient killer(client.host(), client.port());
                    killer.cancel_job(session_id, key);
                }
                catch (...)
                {
                }
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
        }
    });

    try
    {
        client.stream_tokens(session_id, key, [&](const std::string &piece) {
            if (g_agent_interrupt.load(std::memory_order_relaxed))
            {
                return false;
            }
            pending += piece;
            const auto hold_tag_prefix = [](std::string_view text, std::string_view tag) {
                const std::size_t max = std::min(text.size(), tag.size() - 1);
                for (std::size_t size = max; size > 0; --size)
                {
                    if (tag.starts_with(text.substr(text.size() - size)))
                    {
                        return size;
                    }
                }
                return std::size_t{0};
            };
            while (!pending.empty())
            {
                if (!thinking)
                {
                    constexpr std::string_view kOpen = "<think>";
                    const auto open = pending.find(kOpen);
                    if (open == std::string::npos)
                    {
                        const std::size_t hold = hold_tag_prefix(pending, kOpen);
                        if (pending.size() > hold)
                        {
                            emit(std::string_view(pending).substr(0, pending.size() - hold));
                            pending.erase(0, pending.size() - hold);
                        }
                        break;
                    }
                    emit(std::string_view(pending).substr(0, open));
                    pending.erase(0, open + kOpen.size());
                    thinking = true;
                    continue;
                }
                constexpr std::string_view kClose = "</think>";
                const auto close = pending.find(kClose);
                if (close == std::string::npos)
                {
                    const std::size_t hold = hold_tag_prefix(pending, kClose);
                    if (pending.size() > hold)
                    {
                        take_think(std::string_view(pending).substr(0, pending.size() - hold));
                        pending.erase(0, pending.size() - hold);
                    }
                    break;
                }
                take_think(std::string_view(pending).substr(0, close));
                pending.erase(0, close + kClose.size());
                thinking = false;
                close_think();
            }
            return true;
        }, [&](int used, int size) { show_context(state_, used, size); });
    }
    catch (...)
    {
        watcher.request_stop();
        watcher.join();
        close_think();
        if (!state_.debug)
        {
            show_assistant(hidden.finish());
        }
        if (answer_open)
        {
            state_.ui->end();
        }
        throw;
    }
    watcher.request_stop();
    watcher.join();
    if (!thinking)
    {
        emit(pending);
    }
    else
    {
        take_think(pending);
    }
    close_think();
    if (!state_.debug)
    {
        show_assistant(hidden.finish());
    }
    if (answer_open)
    {
        state_.ui->end();
    }
}

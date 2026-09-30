#include "common/tools/tool_execute_command.hpp"

#include <algorithm>
#include <chrono>
#include <cerrno>
#include <cstdint>
#include <fcntl.h>
#include <format>
#include <poll.h>
#include <signal.h>
#include <string_view>
#include <sys/wait.h>
#include <unistd.h>

namespace {

// How often a running command notices Ctrl-C. The child is in its own process
// group, so the terminal signal does not reach it. The TUI reports Ctrl-C as a
// key, and exec reports it as SIGINT. Both set the flag polled here.
constexpr int kPollSliceMs = 100;

std::atomic<const std::atomic<bool> *> g_cancel{nullptr};

bool cancel_requested() {
    const std::atomic<bool> *flag = g_cancel.load(std::memory_order_acquire);
    return flag != nullptr && flag->load(std::memory_order_relaxed);
}

std::string format_elapsed(std::chrono::milliseconds elapsed) {
    const auto ms = std::max<std::int64_t>(elapsed.count(), 0);
    if (ms < 1000) {
        return std::format("{}ms", ms);
    }
    const auto tenths = (ms + 50) / 100;
    return std::format("{}.{}s", tenths / 10, tenths % 10);
}

std::string aborted_message(std::chrono::milliseconds elapsed, const std::string &output) {
    std::string result = std::format("{}{}\nOutput:\n", Tools::kCommandAbortedPrefix, format_elapsed(elapsed));
    result += output.empty() ? "(no output)" : output;
    return result;
}

// Length of one UTF-8 text sequence at index. 0 when the byte is not text.
// incomplete is set when a lead byte is cut off at the end of the buffer.
std::size_t utf8_text_sequence(std::string_view text, std::size_t index, bool &incomplete) {
    incomplete = false;
    if (index >= text.size()) {
        return 0;
    }
    const auto lead = static_cast<unsigned char>(text[index]);
    if (lead == 0) {
        return 0;
    }
    if (lead < 0x80) {
        return 1;
    }
    std::size_t need = 0;
    if (lead >= 0xC2 && lead <= 0xDF) {
        need = 2;
    } else if (lead >= 0xE0 && lead <= 0xEF) {
        need = 3;
    } else if (lead >= 0xF0 && lead <= 0xF4) {
        need = 4;
    } else {
        return 0;
    }
    if (index + need > text.size()) {
        incomplete = true;
        return 0;
    }
    const auto next = static_cast<unsigned char>(text[index + 1]);
    if ((lead == 0xE0 && next < 0xA0) || (lead == 0xED && next >= 0xA0) || (lead == 0xF0 && next < 0x90) ||
        (lead == 0xF4 && next >= 0x90) || (next & 0xC0) != 0x80) {
        return 0;
    }
    for (std::size_t extra = 2; extra < need; ++extra) {
        if ((static_cast<unsigned char>(text[index + extra]) & 0xC0) != 0x80) {
            return 0;
        }
    }
    return need;
}

struct SafeText {
    std::string text;
    bool clipped = false;
};

// Tool results are posted as JSON. Invalid UTF-8 makes that parse fail, so binary
// bytes are written as \xNN and a trailing split character from the size cap is dropped.
SafeText json_safe_command_output(std::string_view raw, bool allow_incomplete_tail) {
    bool binary = false;
    std::size_t kept = 0;
    for (std::size_t index = 0; index < raw.size();) {
        bool incomplete = false;
        const std::size_t size = utf8_text_sequence(raw, index, incomplete);
        if (size > 0) {
            index += size;
            kept = index;
            continue;
        }
        if (incomplete && allow_incomplete_tail) {
            break;
        }
        binary = true;
        break;
    }
    if (!binary) {
        return {std::string(raw.substr(0, kept)), false};
    }

    std::string out = std::format("binary output ({} bytes), non-text bytes shown as \\xNN\n", raw.size());
    bool clipped = false;
    for (const char ch : raw) {
        const auto byte = static_cast<unsigned char>(ch);
        const std::string piece = (byte == '\n' || byte == '\t' || byte == '\r' || (byte >= 0x20 && byte < 0x7F))
                                      ? std::string(1, static_cast<char>(byte))
                                      : std::format("\\x{:02x}", static_cast<unsigned>(byte));
        if (out.size() + piece.size() > MAX_TOOL_OUTPUT_CHARS) {
            clipped = true;
            break;
        }
        out += piece;
    }
    return {std::move(out), clipped};
}

} // namespace

namespace Tools {
    struct execute_command_request {
        // The command to be executed on the client machine
        std::optional<std::string> command;
    };

    void set_execute_command_cancel(const std::atomic<bool> *cancel) {
        g_cancel.store(cancel, std::memory_order_release);
    }

    static void from_json(const nlohmann::json &j, execute_command_request &req) {
        if (j.contains("command")) {
            if (const auto &_1 = j.at("command"); _1.is_string()) {
                req.command = j.at("command").get<std::string>();
            }
        }
    }

    std::string execute_command(const nlohmann::json &args) {
        const execute_command_request req = args.get<execute_command_request>();
        if (!req.command.has_value()) {
            return "error: missing required string argument 'command'";
        }

        constexpr int kTimeoutMs = 180000;
        const auto started = std::chrono::steady_clock::now();
        const auto elapsed_since_start = [&] {
            return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
        };
        if (cancel_requested()) {
            return aborted_message(elapsed_since_start(), {});
        }

        int pipefd[2] = {-1, -1};
        if (pipe(pipefd) != 0) {
            return "error: failed to execute command (pipe failed)";
        }

        const pid_t pid = fork();
        if (pid < 0) {
            close(pipefd[0]);
            close(pipefd[1]);
            return "error: failed to execute command (fork failed)";
        }
        if (pid == 0) {
            setpgid(0, 0);
            const int devnull = open("/dev/null", O_RDONLY);
            if (devnull >= 0) {
                dup2(devnull, STDIN_FILENO);
                close(devnull);
            }
            dup2(pipefd[1], STDOUT_FILENO);
            dup2(pipefd[1], STDERR_FILENO);
            close(pipefd[0]);
            close(pipefd[1]);
            execl("/bin/sh", "sh", "-c", req.command->c_str(), static_cast<char *>(nullptr));
            _exit(127);
        }

        setpgid(pid, pid);
        close(pipefd[1]);

        std::string output;
        bool truncated = false;
        bool timed_out = false;
        bool interrupted = false;
        std::chrono::milliseconds ran{0};
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kTimeoutMs);
        while (true) {
            if (cancel_requested()) {
                ran = elapsed_since_start();
                interrupted = true;
                break;
            }
            const auto remain = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    deadline - std::chrono::steady_clock::now())
                                    .count();
            if (remain <= 0) {
                timed_out = true;
                break;
            }
            pollfd pfd{};
            pfd.fd = pipefd[0];
            pfd.events = POLLIN;
            const int slice = static_cast<int>(std::min<std::int64_t>(remain, kPollSliceMs));
            const int rc = poll(&pfd, 1, slice);
            if (rc < 0) {
                if (errno == EINTR) {
                    continue;
                }
                break;
            }
            if (rc == 0) {
                continue;
            }
            char buffer[512];
            const ssize_t n = read(pipefd[0], buffer, sizeof(buffer));
            if (n == 0) {
                break;
            }
            if (n < 0) {
                if (errno == EINTR) {
                    continue;
                }
                break;
            }
            output.append(buffer, static_cast<size_t>(n));
            if (output.size() > MAX_TOOL_OUTPUT_CHARS) {
                output.resize(MAX_TOOL_OUTPUT_CHARS);
                truncated = true;
                break;
            }
        }
        close(pipefd[0]);

        if (timed_out || truncated || interrupted) {
            if (kill(-pid, SIGKILL) != 0) {
                kill(pid, SIGKILL);
            }
        }
        int status = 0;
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
        }

        const SafeText safe = json_safe_command_output(output, truncated);
        output = safe.text;
        if (safe.clipped) {
            truncated = true;
        }

        if (interrupted) {
            std::string result = aborted_message(ran, output);
            if (truncated) {
                result += "\n\n[output truncated to " + std::to_string(MAX_TOOL_OUTPUT_CHARS) + " characters]";
            }
            return result;
        }
        if (timed_out) {
            std::string result = "error: command timed out after 180s\nOutput:\n";
            result += output.empty() ? "(no output)" : output;
            return result;
        }

        std::string result;
        if (!truncated && WIFEXITED(status)) {
            result = "Exit code: " + std::to_string(WEXITSTATUS(status)) + "\nOutput:\n";
        } else if (!truncated) {
            result = "Exit code: -1\nOutput:\n";
        } else {
            result = "Output:\n";
        }
        result += output.empty() ? "(no output)" : output;
        if (truncated) {
            result += "\n\n[output truncated to " + std::to_string(MAX_TOOL_OUTPUT_CHARS) + " characters]";
        }
        return result;
    }

    Tool create_execute_command_tool() {
        return {
            .name = std::string(kExecuteCommandName),
            .description = std::string(kExecuteCommandDescription),
            .schema_doc = std::string(kExecuteCommandSchema),
            .execute = execute_command,
            .present = [](const nlohmann::json &args) { return tool_arg(args, "command"); }
        };
    }
} // namespace Tools

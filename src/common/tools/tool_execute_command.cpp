#include "common/tools/tool_execute_command.hpp"

#include <algorithm>
#include <chrono>
#include <cerrno>
#include <cstdint>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

namespace Tools {
    struct execute_command_request {
        // The command to be executed on the client machine
        std::optional<std::string> command;
    };

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
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kTimeoutMs);
        while (true) {
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
            const int rc = poll(&pfd, 1, static_cast<int>(std::min<std::int64_t>(remain, 1000)));
            if (rc < 0) {
                if (errno == EINTR) {
                    interrupted = true;
                    break;
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

        if (timed_out || interrupted) {
            std::string result = interrupted ? "error: command interrupted\nOutput:\n"
                                             : "error: command timed out after 180s\nOutput:\n";
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
            .name = "execute_command",
            .description =
                    "Execute a shell / bash command on the local system and return the output and exit code.",
            .schema_doc = "arguments:\n      command: string (the shell command to run)",
            .execute = execute_command
        };
    }
} // namespace Tools

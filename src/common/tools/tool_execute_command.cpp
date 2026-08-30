#include "common/tools/tool_execute_command.hpp"
#include <cstdio>
#include <sys/wait.h>
#include <unistd.h>

namespace Tools {

std::string execute_command(const nlohmann::json & args) {
    // Validate required command field
    if (!args.contains("command") || !args["command"].is_string()) {
        return "error: missing required string argument 'command'";
    }
    std::string cmd = args["command"].get<std::string>();

    // Redirect stderr to stdout (2>&1) to capture both output and error logs
    std::string full_cmd = cmd + " 2>&1";

    FILE * pipe = popen(full_cmd.c_str(), "r");
    if (!pipe) {
        return "error: failed to execute command (popen failed)";
    }

    char buffer[512];
    std::string output;
    bool truncated = false;

    // Read command output in chunks and truncate if limit is exceeded
    while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        output.append(buffer);
        if (output.size() > MAX_TOOL_OUTPUT_CHARS) {
            output.resize(MAX_TOOL_OUTPUT_CHARS);
            truncated = true;
            break;
        }
    }

    int status = pclose(pipe);
    int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;

    std::string result = "Exit code: " + std::to_string(exit_code) + "\nOutput:\n";
    if (output.empty()) {
        result += "(no output)";
    } else {
        result += output;
    }
    if (truncated) {
        result += "\n\n[output truncated to " + std::to_string(MAX_TOOL_OUTPUT_CHARS) + " characters]";
    }
    return result;
}

Tool create_execute_command_tool() {
    return {
        "execute_command",
        "Execute a shell / bash command on the local system and return the output and exit code.",
        "arguments:\n      command: string (the shell command to run)",
        execute_command
    };
}

} // namespace Tools

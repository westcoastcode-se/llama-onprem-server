#include "common/tools/tool_subagent.hpp"
#include <string>
#include <vector>

namespace Tools {

std::string subagent_request_text(const nlohmann::json &args)
{
    if (args.is_string())
    {
        return args.get<std::string>();
    }
    if (!args.is_object())
    {
        return {};
    }
    std::string text;
    if (args.contains("tasks") && args["tasks"].is_array())
    {
        for (const auto &item : args["tasks"])
        {
            if (!item.is_string())
            {
                continue;
            }
            if (!text.empty())
            {
                text.push_back('\n');
            }
            text += item.get<std::string>();
        }
    }
    if (text.empty())
    {
        const char *const keys[] = {"task", "prompt", "instruction", "description"};
        for (const char *key : keys)
        {
            if (args.contains(key) && args[key].is_string())
            {
                text = args[key].get<std::string>();
                break;
            }
        }
    }
    if (args.contains("inherit") && args["inherit"].is_boolean() && args["inherit"].get<bool>())
    {
        if (!text.empty())
        {
            text.push_back('\n');
        }
        text += "inherit: yes";
    }
    return text;
}

Tool create_subagent_tool(SubagentRunner runner) {
    return {
        .name = "sub_agent",
        .description =
                "Delegate a task or multiple sub-tasks to an isolated sub-agent. The sub-agent runs in an independent "
                "context with full tool access (read/write files, execute commands, search code, web search, etc.) and "
                "returns only its final result. Supports executing a single task or multiple tasks in a controlled "
                "sequential order. Always use sub-agents when exploring large codebases, inspecting multiple or large "
                "files, or investigating complex components to keep large reads out of the parent session. "
                "The result is a summary that cites file:line, plus git diff --stat when files changed. "
                "Starts with an empty conversation unless inherit is true. If the result says the context is full, "
                "call again with a smaller task so the answer can be shorter.",
        .schema_doc = "arguments:\n      task: string (one sub-task, or a JSON array of sub-tasks)\n      tasks: "
                          "array (optional list of tasks to execute in order)\n      inherit: boolean (optional, default false; "
                          "copy this conversation into the sub-agent only when the task needs it)",
        .execute = [runner](const nlohmann::json &args) -> std::string {
                if (!runner)
                {
                    return "error: sub-agent runner not configured";
                }
                std::vector<std::string> tasks;

                auto extract_task_from_val = [](const nlohmann::json &val) -> std::string {
                    if (val.is_string())
                    {
                        return val.get<std::string>();
                    }
                    if (val.is_object())
                    {
                        if (val.contains("task") && val["task"].is_string())
                            return val["task"].get<std::string>();
                        if (val.contains("prompt") && val["prompt"].is_string())
                            return val["prompt"].get<std::string>();
                        if (val.contains("instruction") && val["instruction"].is_string())
                            return val["instruction"].get<std::string>();
                        if (val.contains("description") && val["description"].is_string())
                            return val["description"].get<std::string>();
                        if (val.contains("subtask") && val["subtask"].is_string())
                            return val["subtask"].get<std::string>();
                        if (val.contains("sub_task") && val["sub_task"].is_string())
                            return val["sub_task"].get<std::string>();
                        if (val.contains("name") && val["name"].is_string())
                            return val["name"].get<std::string>();
                        if (val.contains("step") && val["step"].is_string())
                            return val["step"].get<std::string>();
                    }
                    return "";
                };

                if (args.is_array())
                {
                    for (const auto &item : args)
                    {
                        std::string t = extract_task_from_val(item);
                        if (!t.empty())
                            tasks.push_back(std::move(t));
                    }
                }
                else if (args.is_object())
                {
                    // Check list/array fields first
                    const char *const array_keys[] = {"tasks", "subtasks", "sub_tasks", "steps", "task"};
                    bool array_found = false;
                    for (const char *key : array_keys)
                    {
                        if (args.contains(key) && args[key].is_array())
                        {
                            array_found = true;
                            for (const auto &item : args[key])
                            {
                                std::string t = extract_task_from_val(item);
                                if (!t.empty())
                                    tasks.push_back(std::move(t));
                            }
                            break;
                        }
                    }

                    if (!array_found)
                    {
                        const char *const str_keys[] = {"task",    "prompt",   "instruction", "description",
                                                        "subtask", "sub_task", "step",        "name"};
                        for (const char *key : str_keys)
                        {
                            if (args.contains(key) && args[key].is_string())
                            {
                                std::string t = args[key].get<std::string>();
                                if (!t.empty())
                                {
                                    tasks.push_back(std::move(t));
                                    break;
                                }
                            }
                        }
                    }
                }
                else if (args.is_string())
                {
                    std::string t = args.get<std::string>();
                    if (!t.empty())
                        tasks.push_back(std::move(t));
                }

                if (tasks.empty())
                {
                    return "error: missing required argument 'task'";
                }

                bool inherit = false;
                if (args.is_object() && args.contains("inherit") && args["inherit"].is_boolean())
                {
                    inherit = args["inherit"].get<bool>();
                }

                if (tasks.size() == 1)
                {
                    return runner(tasks[0], inherit);
                }

                // Execute all tasks sequentially in controlled order
                std::string combined_result;
                for (size_t i = 0; i < tasks.size(); ++i)
                {
                    std::string task_res = runner(tasks[i], inherit);
                    if (task_res.starts_with("error: sub-agent ran out of context"))
                    {
                        return task_res;
                    }
                    if (i > 0)
                    {
                        combined_result += "\n\n";
                    }
                    combined_result += "### Task " + std::to_string(i + 1) + "/" + std::to_string(tasks.size()) + ": " +
                                       tasks[i] + "\n";
                    combined_result += task_res;
                }
                return combined_result;
            },
            .aliases = {"subagent", "delegate_subagent", "delegate_task", "spawn_subagent", "run_subagent", "sub_task",
                        "subtask", "run_task", "task_agent", "tasks", "run_tasks"}};
}

} // namespace Tools

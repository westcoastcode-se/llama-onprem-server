#include "common/tools/tool_write_file.hpp"
#include <filesystem>
#include <fstream>
#include <string>

namespace Tools {

std::string write_file(const nlohmann::json & args) {
    if (!args.contains("path") || !args["path"].is_string()) {
        return "error: missing required string argument 'path'";
    }
    if (!args.contains("content") || !args["content"].is_string()) {
        return "error: missing required string argument 'content'";
    }
    std::string path = args["path"].get<std::string>();
    std::string content = args["content"].get<std::string>();

    try {
        std::filesystem::path p(path);
        // Recursively create parent directories if missing
        if (p.has_parent_path()) {
            std::filesystem::create_directories(p.parent_path());
        }
        const std::filesystem::path tmp = p.string() + ".callisto-tmp";
        {
            std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
            if (!out.is_open()) {
                return "error: failed to open file for writing: " + path;
            }
            out.write(content.data(), static_cast<std::streamsize>(content.size()));
            out.flush();
            if (!out) {
                out.close();
                std::filesystem::remove(tmp);
                return "error: failed to write file: " + path;
            }
        }
        std::filesystem::rename(tmp, p);
        return "success: wrote " + std::to_string(content.size()) + " bytes to " + path;
    } catch (const std::exception & e) {
        return std::string("error writing file: ") + e.what();
    }
}

Tool create_write_file_tool() {
    return {
        .name = "write_file",
        .description = "Write or overwrite a file with given content. Creates parent directories if needed.",
        .schema_doc =
                "arguments:\n      path: string (path to the file)\n      content: string (the full content to write)",
        .execute = write_file
    };
}

} // namespace Tools

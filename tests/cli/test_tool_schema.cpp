//
// File containing tests for prose tool schemas
//

#include "cli/tool_schema.hpp"
#include "../tests.hpp"

/**
 * A prose schema becomes JSON properties. Optional lines are not required.
 */
static int test_tool_schema_from_prose() {
    const auto schema = ToolSchema::from_prose(
        "arguments:\n"
        "      path: string (path to the file)\n"
        "      offset: integer (optional start line)\n"
        "      flag: bool (optional)\n");
    assertEquals("object", schema.value("type", ""));
    assertEquals("string", schema["properties"]["path"].value("type", ""));
    assertEquals("path to the file", schema["properties"]["path"].value("description", ""));
    assertEquals("integer", schema["properties"]["offset"].value("type", ""));
    assertEquals("boolean", schema["properties"]["flag"].value("type", ""));
    assertEquals(1, static_cast<int>(schema["required"].size()));
    assertEquals("path", schema["required"][0].get<std::string>());
    return EXIT_SUCCESS;
}

/**
 * Lines that are not `name: type` are ignored.
 */
static int test_tool_schema_skips_noise() {
    const auto schema = ToolSchema::from_prose("arguments:\n  not a field\n  : string (missing name)\n");
    assertTrue(schema["properties"].empty());
    assertTrue(!schema.contains("required"));
    return EXIT_SUCCESS;
}

/**
 * chat_tool copies the name and stores the schema as a JSON string.
 */
static int test_tool_schema_chat_tool() {
    const Tool tool{
        .name = "read_file",
        .description = "Read a file",
        .schema_doc = "arguments:\n      path: string (path to the file)\n",
        .execute = [](const nlohmann::json &) { return std::string{}; },
    };
    const auto spec = ToolSchema::chat_tool(tool);
    assertEquals("read_file", spec.name);
    assertEquals("Read a file", spec.description);
    const auto parameters = nlohmann::json::parse(spec.parameters);
    assertEquals("string", parameters["properties"]["path"].value("type", ""));
    return EXIT_SUCCESS;
}

/**
 * When the prose has no fields, it is appended to the description.
 */
static int test_tool_schema_chat_tool_keeps_unparsed_prose() {
    const Tool tool{
        .name = "ping",
        .description = "Ping",
        .schema_doc = "free form notes",
        .execute = [](const nlohmann::json &) { return std::string{}; },
    };
    const auto spec = ToolSchema::chat_tool(tool);
    assertTrue(spec.description.find("free form notes") != std::string::npos);
    return EXIT_SUCCESS;
}

/**
 * Run all tool schema tests
 */
int test_tool_schema() {
    RUN_TEST(test_tool_schema_from_prose);
    RUN_TEST(test_tool_schema_skips_noise);
    RUN_TEST(test_tool_schema_chat_tool);
    RUN_TEST(test_tool_schema_chat_tool_keeps_unparsed_prose);
    return EXIT_SUCCESS;
}

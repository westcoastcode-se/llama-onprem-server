cmake_minimum_required(VERSION 4.1)
project(localai)

#
# Properties
#
set(CMAKE_EXPORT_COMPILE_COMMANDS ON) # Required for Zed
string(LENGTH "${CMAKE_SOURCE_DIR}/" SOURCE_PATH_SIZE)

# Vendor & Third Party Libraries
set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build static libraries" FORCE)
set(LLAMA_STATIC ON CACHE BOOL "llama: static link libraries" FORCE)
set(GGML_STATIC ON CACHE BOOL "ggml: static link libraries" FORCE)
add_subdirectory(vendors)

# System dependencies
find_package(CURL REQUIRED)

#
# Target Configuration Helper
#
function(configure_localai_target target)
    set_property(TARGET ${target} PROPERTY CXX_STANDARD 23)
    target_include_directories(${target} PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${LLAMA_INCLUDE_DIR}
        ${CMAKE_SOURCE_DIR}/vendors/llama.cpp/vendor)
    if (CMAKE_BUILD_TYPE STREQUAL "Release")
        if (MSVC)
            target_compile_options(${target} PRIVATE /W4 /O2 /wd4100 /FA /arch:SSE2 /DAI_SOURCE_PATH_SIZE=${SOURCE_PATH_SIZE})
        else ()
            target_compile_options(${target} PRIVATE -Wall -Wextra -O3 -fno-math-errno -fno-trapping-math -Wstrict-overflow -std=c++23 "-DAI_SOURCE_PATH_SIZE=${SOURCE_PATH_SIZE}")
        endif ()
    elseif (CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
        if (MSVC)
            target_compile_options(${target} PRIVATE /W4 /O2 /wd4100 /FA /arch:SSE2 /DAI_SOURCE_PATH_SIZE=${SOURCE_PATH_SIZE})
        else ()
            target_compile_options(${target} PRIVATE -Wall -Wextra -O3  -fno-omit-frame-pointer -Wstrict-overflow -std=c++23 -g -mavx2 "-DAI_SOURCE_PATH_SIZE=${SOURCE_PATH_SIZE}")
            target_link_libraries(${target} PRIVATE  -fno-omit-frame-pointer -Wstrict-overflow -g)
        endif ()
    else ()
        if (MSVC)
            target_compile_options(${target} PRIVATE /W4 /wd4100 /FA /arch:SSE2 /DAI_SOURCE_PATH_SIZE=${SOURCE_PATH_SIZE})
        else ()
            target_compile_options(${target} PRIVATE -Wall -Wextra -fno-omit-frame-pointer -Wstrict-overflow -std=c++23 -g -mavx2 "-DAI_SOURCE_PATH_SIZE=${SOURCE_PATH_SIZE}")
            target_link_libraries(${target} PRIVATE -fno-omit-frame-pointer -Wstrict-overflow -g)
        endif ()
    endif ()
endfunction()

#
# Common Tool Sources
#
set(COMMON_TOOL_SOURCES
    src/common/tools.cpp
    src/common/tool_approval.cpp
    src/common/response_blocks.cpp
    src/common/tools/web_utils.cpp
    src/common/tools/tool_execute_command.cpp
    src/common/tools/tool_read_file.cpp
    src/common/tools/tool_write_file.cpp
    src/common/tools/tool_edit_file.cpp
    src/common/tools/tool_list_directory.cpp
    src/common/tools/tool_file_search.cpp
    src/common/tools/tool_search_text.cpp
    src/common/tools/tool_web_fetch.cpp
    src/common/tools/tool_web_search.cpp
    src/common/tools/tool_subagent.cpp
)

#
# HTTP server. One model, sessions, and a job queue.
#
add_executable(callisto_server
        src/api/models.cpp
        src/server/main.cpp
        src/server/http/routes.cpp
        src/server/jobs/jobs.cpp
        src/server/sessions/sessions.cpp
        src/server/agent/response_parse.cpp
        src/server/agent/model_adapter.cpp
        src/common/log.cpp
        src/server/llm/llm_engine.cpp
        src/server/llm/kv_trim.cpp
        src/server/sessions/tool_history.cpp)
configure_localai_target(callisto_server)
target_include_directories(callisto_server BEFORE PRIVATE
        ${CMAKE_SOURCE_DIR}/vendors/httplib)
target_link_libraries(callisto_server PRIVATE llama-common llama ${CMAKE_THREAD_LIBS_INIT})

#
# Coding-agent client. Tools run here; the server only generates.
# CLI11 parses argv. The interactive session is an FTXUI view.
#
include(FetchContent)
set(FTXUI_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(FTXUI_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(FTXUI_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(FTXUI_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(ftxui
        GIT_REPOSITORY https://github.com/ArthurSonzogni/FTXUI.git
        GIT_TAG v6.1.9
        GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(ftxui)

add_executable(callisto_cli
        src/cli/main.cpp
        src/cli/agent/agent.cpp
        src/cli/agent/agent_session.cpp
        src/cli/agent/agent_commands.cpp
        src/cli/project/project_context.cpp
        src/cli/project/project_git.cpp
        src/cli/project/project_hints.cpp
        src/cli/project/project_ignore.cpp
        src/cli/agent/reply_stream.cpp
        src/cli/servers.cpp
        src/cli/session_store.cpp
        src/cli/tui/system_block.cpp
        src/cli/agent/tool_runner.cpp
        src/cli/tui/theme.cpp
        src/cli/agent/tool_schema.cpp
        src/cli/tui/transcript_scroll.cpp
        src/cli/tui/tui.cpp
        src/cli/tui/tui_input.cpp
        src/cli/tui/tui_menu.cpp
        src/cli/tui/tui_transcript.cpp
        src/cli/rest_client.cpp
        src/api/models.cpp
        src/common/log.cpp
        ${COMMON_TOOL_SOURCES})
configure_localai_target(callisto_cli)
target_include_directories(callisto_cli BEFORE PRIVATE
        ${CMAKE_SOURCE_DIR}/vendors/httplib
        ${CMAKE_SOURCE_DIR}/vendors/cli11)
target_link_libraries(callisto_cli PRIVATE ftxui::component CURL::libcurl ${CMAKE_THREAD_LIBS_INIT})

#
# Unit tests. No model weights required. llama is linked for the chat template.
# tests/tests.hpp provides RUN_TEST, assertEquals, and assertTrue.
#
add_executable(tests
    tests/tests.cpp
    tests/common/test_tools.cpp
    tests/common/test_tool_policy.cpp
    tests/common/test_web_utils.cpp
    tests/common/test_defer.cpp
    tests/common/tools/test_tool_execute_command.cpp
    tests/common/tools/test_tool_read_file.cpp
    tests/common/tools/test_tool_write_file.cpp
    tests/common/tools/test_tool_list_directory.cpp
    tests/common/tools/test_tool_file_search.cpp
    tests/common/tools/test_tool_search_text.cpp
    tests/common/tools/test_tool_subagent.cpp
    tests/cli/test_theme.cpp
    tests/cli/test_tool_schema.cpp
    src/cli/tui/theme.cpp
    tests/cli/test_servers.cpp
    tests/cli/test_project_context.cpp
    tests/cli/test_session_store.cpp
    tests/cli/test_transcript_scroll.cpp
    tests/cli/test_system_block.cpp
    tests/cli/test_context_pressure.cpp
    src/cli/tui/transcript_scroll.cpp
    src/cli/tui/system_block.cpp
    tests/server/test_utf8_stream.cpp
    tests/server/test_token_buffer.cpp
    tests/api/test_models.cpp
    tests/server/test_llama_engine.cpp
    tests/server/test_context_params.cpp
    tests/server/test_session_gc.cpp
    tests/server/test_tool_history.cpp
    src/api/models.cpp
    src/cli/agent/tool_schema.cpp
    src/cli/project/project_context.cpp
    src/cli/project/project_git.cpp
    src/cli/project/project_hints.cpp
    src/cli/project/project_ignore.cpp
    src/cli/servers.cpp
    src/cli/session_store.cpp
    src/common/log.cpp
    src/server/sessions/tool_history.cpp
    src/server/agent/model_adapter.cpp
    src/server/agent/response_parse.cpp
    src/server/jobs/jobs.cpp
    src/server/llm/kv_trim.cpp
    src/server/llm/llm_engine.cpp
    src/server/sessions/sessions.cpp
    ${COMMON_TOOL_SOURCES})
configure_localai_target(tests)
target_link_libraries(tests PRIVATE ftxui::dom CURL::libcurl llama-common llama ${CMAKE_THREAD_LIBS_INIT})

enable_testing()
add_test(NAME tests COMMAND $<TARGET_FILE:tests>)

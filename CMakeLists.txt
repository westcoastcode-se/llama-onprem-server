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
    src/common/tools/web_utils.cpp
    src/common/tools/tool_execute_command.cpp
    src/common/tools/tool_read_file.cpp
    src/common/tools/tool_write_file.cpp
    src/common/tools/tool_apply_patch.cpp
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
        src/server.cpp
        src/server/http/routes.cpp
        src/server/jobs/jobs.cpp
        src/server/sessions/sessions.cpp
        src/server/agent/response_parse.cpp
        src/server/agent/model_adapter.cpp
        src/common/log.cpp
        src/server/llm/llm_engine.cpp
        src/common/tool_history.cpp)
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
        src/cli.cpp
        src/cli/agent.cpp
        src/cli/agent_session.cpp
        src/cli/project_context.cpp
        src/cli/reply_stream.cpp
        src/cli/servers.cpp
        src/cli/session_store.cpp
        src/cli/system_block.cpp
        src/cli/tool_runner.cpp
        src/cli/theme.cpp
        src/cli/tool_schema.cpp
        src/cli/transcript_scroll.cpp
        src/cli/tui.cpp
        src/common/log.cpp
        ${COMMON_TOOL_SOURCES})
configure_localai_target(callisto_cli)
target_include_directories(callisto_cli BEFORE PRIVATE
        ${CMAKE_SOURCE_DIR}/vendors/httplib
        ${CMAKE_SOURCE_DIR}/vendors/cli11)
target_link_libraries(callisto_cli PRIVATE ftxui::component CURL::libcurl ${CMAKE_THREAD_LIBS_INIT})

#
# Unit tests. No model required. callisto_tests also links llama for the chat template.
# Both binaries use tests/tests.hpp (RUN_TEST, assertEquals, assertTrue).
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
    src/cli/theme.cpp
    tests/cli/test_servers.cpp
    tests/cli/test_project_context.cpp
    tests/cli/test_session_store.cpp
    tests/cli/test_transcript_scroll.cpp
    tests/cli/test_system_block.cpp
    src/cli/transcript_scroll.cpp
    src/cli/system_block.cpp
    tests/server/test_utf8_stream.cpp
    tests/server/test_token_buffer.cpp
    tests/api/test_models.cpp
    src/cli/tool_schema.cpp
    src/cli/project_context.cpp
    src/cli/servers.cpp
    src/cli/session_store.cpp
    ${COMMON_TOOL_SOURCES})
configure_localai_target(tests)
target_link_libraries(tests PRIVATE ftxui::dom CURL::libcurl ${CMAKE_THREAD_LIBS_INIT})

add_executable(callisto_tests
        tests/test_llama_engine.cpp
        tests/server/test_context_params.cpp
        tests/server/test_session_gc.cpp
        tests/server/test_tool_history.cpp
        src/server/agent/response_parse.cpp
        src/server/agent/model_adapter.cpp
        src/common/log.cpp
        src/common/tool_history.cpp)
configure_localai_target(callisto_tests)
target_compile_definitions(callisto_tests PRIVATE)
target_link_libraries(callisto_tests PRIVATE llama-common llama ${CMAKE_THREAD_LIBS_INIT})

enable_testing()
add_test(NAME tests COMMAND $<TARGET_FILE:tests>)
add_test(NAME callisto_tests COMMAND $<TARGET_FILE:callisto_tests>)

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
# HTTP server. One model and a job queue. Codex and Copilot use POST /v1/responses.
#
add_executable(callisto_server
        src/api/models.cpp
        src/server/main.cpp
        src/server/options.cpp
        src/server/http/routes.cpp
        src/server/jobs/jobs.cpp
        src/server/agent/response_parse.cpp
        src/server/agent/model_adapter.cpp
        src/common/log.cpp
        src/common/xdg.cpp
        src/server/llm/llm_engine.cpp
        src/server/llm/kv_trim.cpp
        src/server/llm/session_kv_store.cpp
        src/server/responses/responses.cpp)
configure_localai_target(callisto_server)
target_include_directories(callisto_server BEFORE PRIVATE
        ${CMAKE_SOURCE_DIR}/vendors/httplib)
target_link_libraries(callisto_server PRIVATE llama-common llama ${CMAKE_THREAD_LIBS_INIT})

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
    tests/common/test_xdg.cpp
    tests/common/tools/test_tool_execute_command.cpp
    tests/common/tools/test_tool_read_file.cpp
    tests/common/tools/test_tool_write_file.cpp
    tests/common/tools/test_tool_list_directory.cpp
    tests/common/tools/test_tool_file_search.cpp
    tests/common/tools/test_tool_search_text.cpp
    tests/common/tools/test_tool_subagent.cpp
    tests/server/test_utf8_stream.cpp
    tests/server/test_token_buffer.cpp
    tests/api/test_models.cpp
    tests/server/test_llama_engine.cpp
    tests/server/test_session_kv_store.cpp
    tests/server/test_session_disk.cpp
    tests/server/test_server_options.cpp
    tests/server/test_responses.cpp
    src/server/options.cpp
    src/server/responses/responses.cpp
    tests/server/test_context_params.cpp
    tests/server/test_session_gc.cpp
    tests/server/test_tool_history.cpp
    src/api/models.cpp
    src/common/log.cpp
    src/common/xdg.cpp
    src/server/sessions/tool_history.cpp
    src/server/agent/model_adapter.cpp
    src/server/agent/response_parse.cpp
    src/server/jobs/jobs.cpp
    src/server/llm/kv_trim.cpp
    src/server/llm/llm_engine.cpp
    src/server/llm/session_kv_store.cpp
    src/server/sessions/sessions.cpp
    src/server/sessions/session_disk.cpp
    ${COMMON_TOOL_SOURCES})
configure_localai_target(tests)
target_link_libraries(tests PRIVATE CURL::libcurl llama-common llama ${CMAKE_THREAD_LIBS_INIT})

enable_testing()
add_test(NAME tests COMMAND $<TARGET_FILE:tests>)

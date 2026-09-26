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
        src/server/llm/llm_engine.cpp)
configure_localai_target(callisto_server)
target_include_directories(callisto_server BEFORE PRIVATE
        ${CMAKE_SOURCE_DIR}/vendors/httplib)
target_link_libraries(callisto_server PRIVATE llama-common llama ${CMAKE_THREAD_LIBS_INIT})

#
# HTTP agent client. Tools run here; the server only generates.
#
add_executable(callisto_client
        src/client.cpp
        src/common/log.cpp
        ${COMMON_TOOL_SOURCES})
configure_localai_target(callisto_client)
target_include_directories(callisto_client BEFORE PRIVATE
        ${CMAKE_SOURCE_DIR}/vendors/httplib)
target_link_libraries(callisto_client PRIVATE CURL::libcurl ${CMAKE_THREAD_LIBS_INIT})

#
# Coding-agent CLI. CLI11 parses the command line. Tools run locally.
#
add_executable(callisto_cli
        src/cli/main.cpp
        src/cli/agent.cpp
        src/common/log.cpp
        ${COMMON_TOOL_SOURCES})
configure_localai_target(callisto_cli)
target_include_directories(callisto_cli BEFORE PRIVATE
        ${CMAKE_SOURCE_DIR}/vendors/httplib
        ${CMAKE_SOURCE_DIR}/vendors/cli11)
target_link_libraries(callisto_cli PRIVATE CURL::libcurl ${CMAKE_THREAD_LIBS_INIT})

#
# Tool tests. No model required.
#
add_executable(tests
    tests/tests.cpp
    tests/common/test_tools.cpp
    tests/common/tools/test_tool_execute_command.cpp
    ${COMMON_TOOL_SOURCES})
configure_localai_target(tests)
target_link_libraries(tests PRIVATE CURL::libcurl ${CMAKE_THREAD_LIBS_INIT})

#
# Engine tests: template, tool parse, token prefix. No model required.
#
add_executable(callisto_tests
        tests/test_llama_engine.cpp
        src/server/agent/response_parse.cpp
        src/server/agent/model_adapter.cpp
        src/common/log.cpp)
configure_localai_target(callisto_tests)
target_compile_definitions(callisto_tests PRIVATE
        LLAMA_ENGINE_TEMPLATE="${CMAKE_SOURCE_DIR}/src/templates/Qwen3.8-27B.jinja")
target_link_libraries(callisto_tests PRIVATE llama-common llama ${CMAKE_THREAD_LIBS_INIT})

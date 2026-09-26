//
// Created by per on 8/30/26.
//

#ifndef LOCALAI_TESTS_HPP
#define LOCALAI_TESTS_HPP

#ifdef AI_SOURCE_PATH_SIZE
#define AI_SHORT_FILENAME (&(__FILE__[AI_SOURCE_PATH_SIZE]))
#else
#define AI_SHORT_FILENAME (__FILE__)
#endif

#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>

#define RUN_TEST(test)                                                                                                 \
    do {                                                                                                               \
        std::cout << "[TEST] Running " #test << "..." << std::endl;                                                   \
        if (const int _rc = (test)()) {                                                                                \
            std::cerr << "[TEST] " #test " failed" << std::endl;                                                       \
            return _rc;                                                                                                \
        }                                                                                                              \
    } while (0)

#define assertEquals(expected, actual)                                                                                             \
    if ((expected) != (actual)) {                                                                                                  \
        std::cout << "Assertion failed at " << AI_SHORT_FILENAME << ":" << __LINE__ << ": expected=\"" << expected << "\" actual=\"" << actual << "\"" << std::endl; \
        return EXIT_FAILURE;                                                                                           \
    }
#define assertTrue(value)                                                                                                  \
    if (!(value)) {                                                                                                        \
        std::cout << "Assertion failed at " << AI_SHORT_FILENAME << ":" << __LINE__ << ": \"" << #value << "\" is false" << std::endl; \
        return EXIT_FAILURE;                                                                                           \
    }

// A private directory for one test. The caller removes it, usually with defer().
inline std::filesystem::path make_temp_dir(const char *name)
{
    static int seq = 0;
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto dir = std::filesystem::temp_directory_path() /
                     (std::string("callisto-") + name + "-" + std::to_string(stamp) + "-" + std::to_string(++seq));
    std::filesystem::create_directories(dir);
    return dir;
}

inline void write_test_file(const std::filesystem::path &path, std::string_view text)
{
    if (path.has_parent_path())
    {
        std::filesystem::create_directories(path.parent_path());
    }
    std::ofstream out(path, std::ios::binary);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
}

#endif //LOCALAI_TESTS_HPP

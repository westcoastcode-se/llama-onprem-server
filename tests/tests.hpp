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
#include <thread>

#define RUN_TEST(test) std::cout << "[TEST] Running " #test << "..." << std::endl; test()

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

#endif //LOCALAI_TESTS_HPP

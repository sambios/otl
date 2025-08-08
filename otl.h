//
// Created by yuan on 1/22/21.
//
#ifndef OTL_H
#define OTL_H

#include <stdio.h>
#include <stdlib.h>
#include <memory.h>
#include <assert.h>

#include <iostream>
#include <string>
#include <memory>
#include <unordered_map>
#include <vector>
#include <chrono>

#include <sys/time.h>

#ifdef __linux__
#include <unistd.h>
#endif

// 检测编译器是否支持C++17的std::filesystem
#if __cplusplus >= 201703L
    #if defined(__GNUC__) && __GNUC__ < 9
        // GCC 8支持C++17但需要链接stdc++fs库，且位于experimental命名空间
        #ifdef __linux__
                #include <experimental/filesystem>
                namespace fs = std::experimental::filesystem;
        #endif

   #else
        // 其他支持C++17的编译器（GCC9+、Clang7+、MSVC2017+）
        #include <filesystem>
        namespace fs = std::filesystem;
    #endif
#else
    // C++17之前的版本，使用experimental版本
    #ifdef __linux__
        #include <experimental/filesystem>
        namespace fs = std::experimental::filesystem;
    #endif
#endif

/**
 * @brief Get the lower 16 bits of a 32-bit integer
 * @param x The 32-bit integer to extract from
 * @return The lower 16 bits as uint16_t
 */
#define OTL_GET_INT32_LOW16(x)  ((uint16_t)((x) & 0x0000FFFF))

/**
 * @brief Get the higher 16 bits of a 32-bit integer
 * @param x The 32-bit integer to extract from
 * @return The higher 16 bits as uint16_t
 */
#define OTL_GET_INT32_HIGH16(x) ((uint16_t)(((x) >> 16) & 0x0000FFFF))

/**
 * @brief Set the lower 16 bits of a 32-bit integer while preserving the higher 16 bits
 * @param x The 32-bit integer to modify
 * @param low The 16-bit value to set in the lower position
 */
#define OTL_SET_INT32_LOW16(x, low)  ((x) = ((x) & 0xFFFF0000) | ((int32_t)(low) & 0x0000FFFF))

/**
 * @brief Set the higher 16 bits of a 32-bit integer while preserving the lower 16 bits
 * @param x The 32-bit integer to modify
 * @param high The 16-bit value to set in the higher position
 */
#define OTL_SET_INT32_HIGH16(x, high) ((x) = ((x) & 0x0000FFFF) | ((int32_t)(high) << 16))

/**
 * @brief Combine high 16-bit and low 16-bit values into a 32-bit integer
 * @param high The 16-bit value to be placed in the higher position
 * @param low The 16-bit value to be placed in the lower position
 * @return A 32-bit integer formed by combining high and low values
 */
#define OTL_MAKE_INT32(high, low) \
    ((int32_t)(((uint32_t)(high) << 16) | ((uint32_t)(low) & 0x0000FFFF)))

#include "otl_timer.h"
#include "otl_image.h"
#include "otl_string.h"

#endif //OTL_H

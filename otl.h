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

// 检测编译器是否支持C++17的std::filesystem
#if __cplusplus >= 201703L
    #if defined(__GNUC__) && __GNUC__ < 9
        // GCC 8支持C++17但需要链接stdc++fs库，且位于experimental命名空间
        #include <experimental/filesystem>
        namespace fs = std::experimental::filesystem;
#else
// 其他支持C++17的编译器（GCC9+、Clang7+、MSVC2017+）
#include <filesystem>
namespace fs = std::filesystem;
#endif
#else
// C++17之前的版本，使用experimental版本
#include <experimental/filesystem>
namespace fs = std::experimental::filesystem;
#endif


#include "otl_timer.h"
#include "otl_image.h"
#include "otl_string.h"
#include "stream_decode.h"

#endif //OTL_H

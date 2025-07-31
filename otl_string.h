//===----------------------------------------------------------------------===//
//
// Copyright (C) 2022 Sophgo Technologies Inc.  All rights reserved.
//
// SOPHON-PIPELINE is licensed under the 2-Clause BSD License except for the
// third-party components.
//
//===----------------------------------------------------------------------===//

#ifndef OTL_STRING_H
#define OTL_STRING_H

#include <string>
#include <vector>

namespace otl {
    std::string replaceHomeDirectory(const std::string& path);
    std::vector<std::string> split(std::string str, std::string pattern);
    bool startWith(const std::string &str, const std::string &head);
    std::string fileNameFromPath(const std::string& path, bool hasExt);
    std::string fileExtFromPath(const std::string& str);
    std::string format(const char *pszFmt, ...);
    std::string base64Enc(const void *data, size_t sz);
    std::string base64Dec(const void *data, size_t sz);


}

#endif // OTL_STRING_H

//
// Created by FengYijia on 26-3-1.
//

#ifndef STRING_UTILS_H
#define STRING_UTILS_H

#include <string>
#include <vector>
#include <algorithm>

namespace str_utils {

    // 1. 占位符替换：将 fmt 中的 {} 依次替换为 args 中的内容
    inline std::string format(std::string fmt, const std::vector<std::string>& args) {
        for (const auto& arg : args) {
            size_t pos = fmt.find("{}");
            if (pos != std::string::npos) {
                fmt.replace(pos, 2, arg);
            }
        }
        return fmt;
    }

    // 2. 简单的字符串裁切（可选，后续解析 JSON 可能用到）
    inline std::string trim(std::string s) {
        s.erase(s.begin(), std::find_if(s.begin(), s.end(), [](unsigned char ch) {
            return !std::isspace(ch);
        }));
        s.erase(std::find_if(s.rbegin(), s.rend(), [](unsigned char ch) {
            return !std::isspace(ch);
        }).base(), s.end());
        return s;
    }
}

#endif //STRING_UTILS_H

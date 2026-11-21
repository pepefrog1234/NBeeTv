// 命令列工具共用的參數解析：--名稱 值 或 --名稱=值，其餘是位置參數。
#pragma once

#include <cstdlib>
#include <map>
#include <string>
#include <vector>

namespace nbtv::tools {

struct Args {
    std::map<std::string, std::string> options;
    std::vector<std::string> positional;

    // flags 是不帶值的選項名稱
    Args(int argc, char** argv, const std::vector<std::string>& flags = {}) {
        for (int i = 1; i < argc; ++i) {
            const std::string a = argv[i];
            if (a.rfind("--", 0) == 0) {
                const std::size_t eq = a.find('=');
                if (eq != std::string::npos) {  // --名稱=值
                    options[a.substr(2, eq - 2)] = a.substr(eq + 1);
                    continue;
                }
                const std::string name = a.substr(2);
                bool is_flag = false;
                for (const std::string& f : flags) is_flag = is_flag || f == name;
                if (is_flag || i + 1 >= argc) options[name] = "1";
                else options[name] = argv[++i];
            } else {
                positional.push_back(a);
            }
        }
    }
    bool has(const std::string& name) const { return options.count(name) != 0; }
    std::string str(const std::string& name, const std::string& fallback) const {
        const auto it = options.find(name);
        return it == options.end() ? fallback : it->second;
    }
    double num(const std::string& name, double fallback) const {
        const auto it = options.find(name);
        return it == options.end() ? fallback : std::atof(it->second.c_str());
    }
    int integer(const std::string& name, int fallback) const { return static_cast<int>(num(name, fallback)); }
};

}  // namespace nbtv::tools

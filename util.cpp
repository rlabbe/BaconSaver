#include "util.h"
#include <Shlwapi.h>
#include <sstream>

#pragma comment(lib, "shlwapi.lib")

std::vector<std::string> split_lines(const std::string& s) {
    std::vector<std::string> out;
    std::istringstream ss(s);
    std::string line;
    while (std::getline(ss, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        out.push_back(line);
    }
    return out;
}

bool fnmatch(const std::string& pattern, const std::string& str) {
    if (pattern.find('[') != std::string::npos)
        return PathMatchSpecA(str.c_str(), pattern.c_str()) == TRUE;
    size_t pi = 0, si = 0, pstar = std::string::npos, sstar = 0;
    while (si < str.size()) {
        if (pi < pattern.size() && (pattern[pi] == '?' || pattern[pi] == str[si])) {
            ++pi;
            ++si;
        } else if (pi < pattern.size() && pattern[pi] == '*') {
            pstar = pi++;
            sstar = si;
        } else if (pstar != std::string::npos) {
            pi = pstar + 1;
            si = ++sstar;
        } else {
            return false;
        }
    }
    while (pi < pattern.size() && pattern[pi] == '*')
        ++pi;
    return pi == pattern.size();
}

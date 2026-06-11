#include "ignore_filter.h"
#include "util.h"
#include <algorithm>
#include <fstream>

IgnoreFilter::IgnoreFilter(const fs::path& pattern_file) : _file(pattern_file) {
    reload();
}

std::vector<std::string> IgnoreFilter::patterns() const {
    auto result = _component_patterns;
    result.insert(result.end(), _path_patterns.begin(), _path_patterns.end());
    return result;
}

void IgnoreFilter::reload() {
    _component_patterns.clear();
    _path_patterns.clear();
    if (!fs::exists(_file))
        return;

    std::ifstream in(_file);
    std::string line;
    while (std::getline(in, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
            line.pop_back();
        if (line.empty() || line[0] == '#')
            continue;
        auto p = line;
        if (!p.empty() && p.back() == '/')
            p.pop_back();
        if (p.find('/') != std::string::npos)
            _path_patterns.push_back(p);
        else
            _component_patterns.push_back(p);
    }
}

void IgnoreFilter::set_patterns(const std::vector<std::string>& patterns) {
    {
        std::ofstream out(_file);
        out << "# BaconSaver ignore patterns\n";
        out << "# Pattern without / matches any path component; with / matches full path.\n";
        out << "# Wildcards: * ? [seq]   Comments: lines starting with #\n\n";
        for (auto& p : patterns)
            out << p << '\n';
    }
    reload();
}

bool IgnoreFilter::is_ignored(const std::string& rel_path) const {
    auto norm = fs::path(rel_path);
    for (auto part_it = norm.begin(); part_it != norm.end(); ++part_it)
        for (auto& always : always_ignored)
            if (*part_it == always)
                return true;

    auto parts = norm;
    for (auto& pat : _component_patterns)
        for (auto part_it = parts.begin(); part_it != parts.end(); ++part_it)
            if (fnmatch(pat, part_it->string()))
                return true;

    std::string n = rel_path;
    std::replace(n.begin(), n.end(), '\\', '/');
    for (auto& pat : _path_patterns)
        if (fnmatch(pat, n))
            return true;

    return false;
}


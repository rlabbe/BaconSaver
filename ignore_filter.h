#pragma once
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

class IgnoreFilter {
public:
    explicit IgnoreFilter(const fs::path& pattern_file);

    const fs::path& file() const { return _file; }
    std::vector<std::string> patterns() const;
    bool is_ignored(const std::string& rel_path) const;

    void set_patterns(const std::vector<std::string>& patterns);
    void reload();

private:
    fs::path _file;
    std::vector<std::string> _component_patterns;
    std::vector<std::string> _path_patterns;
};

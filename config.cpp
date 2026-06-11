#include "config.h"
#include "util.h"
#include <fstream>
#include <sstream>

presets_t g_presets = {
    { "General", { ".svn", "x64", "*~", "*.TMP" } },
    { "C++", { ".vs", "x64", "Debug", "Release", "*.suo", "*.user", "*.sdf", "*.opensdf", "*.dll", "*.lib", "*.exe", "*.pdb", "*.ilk", "*.exp", "*.obj" } },
    { "Python", { "__pycache__", "*.pyc", ".mypy_cache", ".pytest_cache", "*.egg-info", ".venv", "venv", ".ipynb_checkpoints" } },
};

bool load_config(json::value& out) {
    std::ifstream in(config_path(), std::ios::binary);
    if (!in)
        return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    return json::parse(ss.str(), out);
}

void save_config(const json::value& v) {
    std::ofstream out(config_path(), std::ios::binary);
    out << json::dump(v, 2);
}

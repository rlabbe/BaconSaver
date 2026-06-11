#pragma once
#include "json.h"
#include <string>
#include <vector>

using presets_t = std::vector<std::pair<std::string, std::vector<std::string>>>;
extern presets_t g_presets;

bool load_config(json::value& out);
void save_config(const json::value& v);

#pragma once
#include "sig_scan.h"
#include <nlohmann/json.hpp>
namespace cs2bc::gamedata {
// Loads the existing native configuration and resolves non-Schema offsets.
bool Load(void* serverIface, nlohmann::json& gd, sig::ModuleInfo& serverModule, char* error, size_t maxlen);
} // namespace cs2bc::gamedata

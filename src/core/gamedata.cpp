#include "core/gamedata.h"
#include "platform.h"
#include "version_targets.h"
#include <cstdio>
#include <string>
namespace cs2bc::gamedata {
namespace {

// addons/<name>/bin/<platform>/<lib> -> up 3 dirs -> addons/<name>/gamedata.json
std::string ComputeGamedataPath()
{
    std::string p = cs2bc::SelfModulePath();
    if (p.empty()) return "";
    for (int i = 0; i < 3; ++i)
    {
        size_t slash = p.find_last_of("/\\");
        if (slash == std::string::npos) return "";
        p.resize(slash);
    }
    return p + "/gamedata.json";
}

} // namespace

// Locates gamedata relative to this plugin and validates its server module.
bool Load(void* serverIface, nlohmann::json& gd, sig::ModuleInfo& serverModule, char* error, size_t maxlen)
{
    std::string gamedataPath = ComputeGamedataPath();
    if (gamedataPath.empty())
    {
        std::snprintf(error, maxlen, "Failed to compute gamedata.json path");
        return false;
    }

    if (!cs2bc::sig::LoadGamedata(gamedataPath.c_str(), gd))
    {
        std::snprintf(error, maxlen, "Failed to load gamedata: %s", gamedataPath.c_str());
        return false;
    }

    serverModule = cs2bc::sig::ModuleFromInterfacePtr(serverIface);
    if (!serverModule)
    {
        std::snprintf(error, maxlen, "ModuleFromInterfacePtr returned null");
        return false;
    }

    // Resolve non-Schema offsets before installing hooks that read targets
    cs2bc::targets::LoadFromGamedata(gd);

    return true;
}
} // namespace cs2bc::gamedata

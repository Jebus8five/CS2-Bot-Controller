// BotController native Metamod:Source plugin entry point.

#include "plugin.h"

#include <cstdio>
#include <string>

#include <eiface.h>
#include <icvar.h>
#include <convar.h>
#include <interfaces/interfaces.h>
#include <networksystem/inetworkmessages.h>
#include <tier0/dbg.h>

#include <nlohmann/json.hpp> // NOLINT(misc-include-cleaner)

#include "ISmmPluginExt.h"
#include "WeaponLocker.h"
#include "BotController.h"
#include "BuyController.h"
#include "BuyControllerState.h"
#include "InputInjector.h"
#include "MotionRecorder.h"
#include "VoiceSender.h"
#include "dispatch.h"
#include "WeaponLockerState.h"
#include "BotControllerState.h"
#include "commands.h"
#include "sig_scan.h"
#include "schema_resolver.h"
#include "platform.h"
#include "ProjectileBirthAlign.h"
#include "version_targets.h"

#define VERSION_STRING  "v" SEMVER " @ " GITHUB_SHA
#define BUILD_TIMESTAMP __DATE__ " " __TIME__

PLUGIN_EXPOSE(cs2bc::BotControllerPlugin, cs2bc::g_plugin);

namespace cs2bc {

BotControllerPlugin g_plugin;

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

bool BotControllerPlugin::Load(PluginId id, ISmmAPI* ismm, char* error, size_t maxlen, bool /*late*/)
{
    PLUGIN_SAVEVARS();

    if (!KHook::__exported__khook)
    {
        std::snprintf(error, maxlen, "Metamod with KHook support is required");
        return false;
    }

    g_pCVar = static_cast<ICvar*>(ismm->GetEngineFactory()(CVAR_INTERFACE_VERSION, nullptr));
    if (!g_pCVar)
    {
        std::snprintf(error, maxlen, "Failed to get ICvar (%s) via engine factory", CVAR_INTERFACE_VERSION);
        return false;
    }

    char schemaError[256] = { 0 };
    if (!cs2bc::schema::Init(schemaError, sizeof(schemaError)))
    {
        std::snprintf(error, maxlen, "Schema initialization failed: %s", schemaError);
        return false;
    }
    if (!cs2bc::targets::LoadFromSchema(schemaError, sizeof(schemaError)))
    {
        cs2bc::schema::Reset();
        std::snprintf(error, maxlen, "Schema target resolution failed: %s", schemaError);
        return false;
    }
    if (cs2bc::projectile_birth_align::ConfigureOffsets(cs2bc::targets::g_projectileInitialPosition,
                                                        cs2bc::targets::g_projectileInitialVelocity) != 0)
    {
        Warning("[BotController] projectile birth alignment offsets unavailable\n");
    }
    ConVar_Register(FCVAR_RELEASE | FCVAR_GAMEDLL);

    // IVEngineServer2::ClientCommand
    cs2bc::dispatch::g_engine = static_cast<IVEngineServer2*>(ismm->GetEngineFactory()(INTERFACEVERSION_VENGINESERVER, nullptr));
    if (!cs2bc::dispatch::g_engine)
    {
        std::snprintf(error, maxlen, "Failed to get IVEngineServer2 (%s)", INTERFACEVERSION_VENGINESERVER);
        return false;
    }

    // Need ISource2GameClients only as the anchor for sig-scan
    void* serverIface = ismm->GetServerFactory()(INTERFACEVERSION_SERVERGAMECLIENTS, nullptr);
    if (!serverIface)
    {
        std::snprintf(error, maxlen, "Failed to get ISource2GameClients (%s)", INTERFACEVERSION_SERVERGAMECLIENTS);
        return false;
    }

    // Engine interface used by console command output (ClientPrintf).
    cs2bc::commands::g_engine = cs2bc::dispatch::g_engine;

    // Server-side command executor for issuing bot "buy" commands.
    cs2bc::dispatch::g_gameClients = static_cast<ISource2GameClients*>(serverIface);

    // NetworkMessages lets the C ABI send recorded voice frames to clients.
    auto* networkMessages = static_cast<INetworkMessages*>(ismm->GetEngineFactory()(NETWORKMESSAGES_INTERFACE_VERSION, nullptr));
    if (!networkMessages)
    {
        networkMessages = static_cast<INetworkMessages*>(ismm->GetServerFactory()(NETWORKMESSAGES_INTERFACE_VERSION, nullptr));
    }
    cs2bc::voice_sender::SetInterfaces(cs2bc::dispatch::g_engine, networkMessages);
    if (!networkMessages)
    {
        Warning("[BotController] network messages interface unavailable; voice send disabled\n");
    }

    std::string gamedataPath = ComputeGamedataPath();
    if (gamedataPath.empty())
    {
        std::snprintf(error, maxlen, "Failed to compute gamedata.json path");
        return false;
    }

    nlohmann::json gd;
    if (!cs2bc::sig::LoadGamedata(gamedataPath.c_str(), gd))
    {
        std::snprintf(error, maxlen, "Failed to load gamedata: %s", gamedataPath.c_str());
        return false;
    }

    cs2bc::sig::ModuleInfo serverModule = cs2bc::sig::ModuleFromInterfacePtr(serverIface);
    if (!serverModule)
    {
        std::snprintf(error, maxlen, "ModuleFromInterfacePtr returned null");
        return false;
    }

    // Resolve non-Schema offsets before installing hooks that read targets
    cs2bc::targets::LoadFromGamedata(gd);

    if (!cs2bc::weapon_locker_hooks::Install(gd, serverModule, error, maxlen)) return false;

    if (!cs2bc::bot_controller_hooks::Install(gd, serverModule, error, maxlen))
    {
        cs2bc::weapon_locker_hooks::Remove();
        return false;
    }

    // BuyController is optional; missing sig only disables buy control
    char buyErr[256] = { 0 };
    if (!cs2bc::buy_controller_hooks::Install(gd, serverModule, buyErr, sizeof(buyErr)))
    {
        Warning("[BotController] BuyController::Install failed (%s); bot buy control disabled\n", buyErr);
    }

    // movement hooks for record/replay
    char injErr[256] = { 0 };
    if (!cs2bc::input_injector::Install(gd, serverModule, injErr, sizeof(injErr)))
    {
        Warning("[BotController] InputInjector::Install failed (%s); record/replay movement will be a no-op\n", injErr);
    }

    return true;
}

// Accepts a plugin pause request.
bool BotControllerPlugin::Pause(char*, size_t) { return true; }

// Accepts a plugin resume request.
bool BotControllerPlugin::Unpause(char*, size_t) { return true; }

// No additional cross-plugin initialization is required.
void BotControllerPlugin::AllPluginsLoaded() {}

// Returns plugin author metadata.
const char* BotControllerPlugin::GetAuthor() { return "XBribo(๑•.•๑)"; }
// Returns the plugin name.
const char* BotControllerPlugin::GetName() { return "BotController"; }
// Returns the plugin description.
const char* BotControllerPlugin::GetDescription() { return "Record & Replay and Control CS2 bots."; }
// Returns the plugin project URL.
const char* BotControllerPlugin::GetURL() { return ""; }
// Returns the plugin license.
const char* BotControllerPlugin::GetLicense() { return "AGPL-3.0"; }
// Returns the version supplied by the build.
const char* BotControllerPlugin::GetVersion() { return VERSION_STRING; }
// Returns the compilation date and time.
const char* BotControllerPlugin::GetDate() { return BUILD_TIMESTAMP; }
// Returns the plugin log tag.
const char* BotControllerPlugin::GetLogTag() { return "BC"; }

bool BotControllerPlugin::Unload(char* /*error*/, size_t /*maxlen*/)
{
    // Drain movement callbacks before releasing their recording and replay state.
    cs2bc::input_injector::Remove();
    cs2bc::motion_recorder::ClearAll();
    cs2bc::projectile_birth_align::Clear();
    cs2bc::buy_controller_hooks::Remove();
    cs2bc::buy_controller_state::ClearAll();
    cs2bc::bot_controller_hooks::Remove();
    cs2bc::weapon_locker_hooks::Remove();
    cs2bc::weapon_locker_state::ClearAll();
    cs2bc::bot_controller_state::ClearAllAll();
    cs2bc::bot_controller_state::ClearAllAim();
    cs2bc::dispatch::g_engine = nullptr;
    cs2bc::dispatch::g_gameClients = nullptr;
    cs2bc::voice_sender::SetInterfaces(nullptr, nullptr);
    cs2bc::commands::g_engine = nullptr;
    cs2bc::schema::Reset();
    ConVar_Unregister();
    g_pCVar = nullptr;
    return true;
}

} // namespace cs2bc

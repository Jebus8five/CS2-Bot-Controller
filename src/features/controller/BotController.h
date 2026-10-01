// MinHook install/remove for CCSBot Update/Upkeep.

#pragma once

#include <string>

#include <nlohmann/json.hpp>
#include "core/memory_module.h"

namespace cs2bc {
namespace bot_controller_hooks {
// Resolve sigs and install detours.
bool Install(const nlohmann::json& gd, const modules::ModuleInfo& serverModule, char* errorOut, size_t errorOutLen);

// Disable + remove detours.
void Remove();

const char* Status();

void* UpdateAddress();
void* UpkeepAddress();
void* UpdateLookAnglesAddress();

// Writes a pawn's eye angles through the engine's own SetEyeAngles path.
// Generic primitive, independent of any specific caller's semantics --
// replay and live aim control both use this.
bool ApplyEyeAngles(void* pawn, float pitch, float yaw);

// Last CCSBot* seen in Update for this slot, or nullptr. Used to read
// the bot's BotProfile by slot.
void* BotForSlot(int slot);

// Resolves a live bot's pawn for this slot and writes its eye angles via
// ApplyEyeAngles above. Fails closed: a missing/stale bot, a missing
// pawn, or a slot mismatch all return false without writing anything.
// Independent of Lock state and of Gate3/replay -- callers decide
// separately whether to suppress the bot's own aim via Lock(Aim).
bool SetEyeAnglesForSlot(int slot, float pitch, float yaw);
} // namespace bot_controller_hooks
} // namespace cs2bc

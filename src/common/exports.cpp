// C-ABI exports for CounterStrikeSharp P/Invoke.

#include "dispatch.h"
#include "MotionRecorder.h"
#include "InputInjector.h"
#include "BuyControllerState.h"
#include "BotProfile.h"
#include "VoiceSender.h"
#include "ProjectileBirthAlign.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#ifdef _WIN32
#define BC_EXPORT __declspec(dllexport)
#else
#define BC_EXPORT __attribute__((visibility("default")))
#endif

extern "C" BC_EXPORT int BotController_Lock(int slot, int kind, int arg)
{
    return bot_controller::dispatch::Lock(slot, static_cast<bot_controller::LockKind>(kind), arg);
}

extern "C" BC_EXPORT int BotController_Unlock(int slot, int kind)
{
    return bot_controller::dispatch::Unlock(slot, static_cast<bot_controller::LockKind>(kind));
}

extern "C" BC_EXPORT int BotController_UnlockAll(int kind)
{
    return bot_controller::dispatch::UnlockAll(static_cast<bot_controller::LockKind>(kind));
}

extern "C" BC_EXPORT int BotController_IsLocked(int slot, int kind)
{
    return bot_controller::dispatch::IsLocked(slot, static_cast<bot_controller::LockKind>(kind));
}

extern "C" BC_EXPORT int BotController_GetVersion() { return 20; }

// Configures native projectile birth fields for the current server build
extern "C" BC_EXPORT int BotController_SetProjectileBirthAlignOffsets(int initialPositionOffset, int initialVelocityOffset)
{
    return bot_controller::projectile_birth_align::ConfigureOffsets(initialPositionOffset, initialVelocityOffset);
}

// Queues one projectile's recorded birth position and velocity
extern "C" BC_EXPORT int
BotController_QueueProjectileBirthAlign(uint64_t entityPtr, float posX, float posY, float posZ, float velX, float velY, float velZ)
{
    return bot_controller::projectile_birth_align::Queue(entityPtr, posX, posY, posZ, velX, velY, velZ);
}

// Clears pending native projectile alignment writes
extern "C" BC_EXPORT int BotController_ClearProjectileBirthAlign() { return bot_controller::projectile_birth_align::Clear(); }

// Returns native projectile alignment diagnostics
extern "C" BC_EXPORT int BotController_GetProjectileBirthAlignStatus(bot_controller::projectile_birth_align::Status* out, int size)
{
    return bot_controller::projectile_birth_align::GetStatus(out, size);
}

// Create an independently cancellable usercmd injection
extern "C" BC_EXPORT int64_t BotController_InjectUsercmd(int slot, uint64_t buttonMask, int durationMs)
{
    return bot_controller::input_injector::InjectUsercmd(slot, buttonMask, durationMs);
}

// Create an independently cancellable persistent analog movement override
extern "C" BC_EXPORT int64_t BotController_StartUsercmdMovement(int slot, float forwardMove, float leftMove)
{
    return bot_controller::input_injector::StartUsercmdMovement(slot, forwardMove, leftMove);
}

// Update one persistent analog movement override
extern "C" BC_EXPORT int BotController_UpdateUsercmdMovement(int slot, int64_t movementId, float forwardMove, float leftMove)
{
    return bot_controller::input_injector::UpdateUsercmdMovement(slot, movementId, forwardMove, leftMove) ? 0 : -1;
}

// Cancel one persistent analog movement override
extern "C" BC_EXPORT int BotController_CancelUsercmdMovement(int slot, int64_t movementId)
{
    return bot_controller::input_injector::CancelUsercmdMovement(slot, movementId) ? 0 : -1;
}

// Cancel one usercmd injection by its token
extern "C" BC_EXPORT int BotController_CancelUsercmdInjection(int slot, int64_t injectionId)
{
    return bot_controller::input_injector::CancelUsercmdInjection(slot, injectionId) ? 0 : -1;
}

// Suppress selected usercmd buttons for a fixed duration
extern "C" BC_EXPORT int BotController_SuppressUsercmd(int slot, uint64_t buttonMask, int durationMs)
{
    return bot_controller::input_injector::SuppressUsercmd(slot, buttonMask, durationMs) ? 0 : -1;
}

// Create an independently cancellable persistent usercmd suppression
extern "C" BC_EXPORT int64_t BotController_StartUsercmdSuppression(int slot, uint64_t buttonMask)
{
    return bot_controller::input_injector::StartUsercmdSuppression(slot, buttonMask);
}

// Cancel one persistent usercmd suppression by its token
extern "C" BC_EXPORT int BotController_CancelUsercmdSuppression(int slot, int64_t suppressionId)
{
    return bot_controller::input_injector::CancelUsercmdSuppression(slot, suppressionId) ? 0 : -1;
}

// Return 1 when the plugin can allocate and send voice net messages.
extern "C" BC_EXPORT int BotController_CanSendVoice() { return bot_controller::voice_sender::IsAvailable() ? 1 : 0; }

// Return 0 when voice sending is ready, otherwise a negative setup code.
extern "C" BC_EXPORT int BotController_GetVoiceStatus() { return bot_controller::voice_sender::GetStatus(); }

// Send one encoded Opus voice frame to a recipient player slot.
extern "C" BC_EXPORT int BotController_SendVoiceFrame(int recipientSlot,
                                                      int senderClient,
                                                      uint64_t senderXuid,
                                                      const uint8_t* audio,
                                                      int audioBytes,
                                                      int sampleRate,
                                                      float voiceLevel,
                                                      int sequenceBytes,
                                                      int sectionNumber,
                                                      int uncompressedSampleOffset,
                                                      uint32_t numPackets,
                                                      const uint32_t* packetOffsets,
                                                      int packetOffsetCount,
                                                      int tick,
                                                      int audibleMask)
{
    return bot_controller::voice_sender::SendVoiceFrame(recipientSlot, senderClient, senderXuid, audio, audioBytes, sampleRate, voiceLevel,
                                                        sequenceBytes, sectionNumber, uncompressedSampleOffset, numPackets, packetOffsets,
                                                        packetOffsetCount, tick, audibleMask);
}

// Read a bot's BotProfile by slot. 0 ok / -1 no live bot or null profile.
extern "C" BC_EXPORT int BotController_GetProfile(int slot, bot_controller::BotProfileData* out)
{
    if (!out) return -1;
    return bot_controller::bot_profile::ReadProfile(slot, *out) ? 0 : -1;
}

// ---- Bot buy plans ----

namespace {

// Split a space/comma separated alias string into tokens.
std::vector<std::string> SplitAliases(const char* csv)
{
    std::vector<std::string> out;
    if (!csv) return out;
    std::string cur;
    for (const char* p = csv; *p; ++p)
    {
        char c = *p;
        if (c == ' ' || c == ',' || c == '\t')
        {
            if (!cur.empty())
            {
                out.push_back(cur);
                cur.clear();
            }
        }
        else
            cur.push_back(c);
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

} // namespace

// Set a slot's buy plan from a space/comma separated alias list. 0 ok.
extern "C" BC_EXPORT int BotController_SetBuyPlan(int slot, const char* aliases)
{
    if (slot < 0 || slot >= bot_controller::buy_controller_state::kMaxSlots) return -2;
    bot_controller::buy_controller_state::Set(slot, SplitAliases(aliases), false);
    return 0;
}

// Mark a slot to buy nothing this round. 0 ok.
extern "C" BC_EXPORT int BotController_SetBuySkip(int slot)
{
    if (slot < 0 || slot >= bot_controller::buy_controller_state::kMaxSlots) return -2;
    bot_controller::buy_controller_state::Set(slot, {}, true);
    return 0;
}

extern "C" BC_EXPORT int BotController_ClearBuyPlan(int slot)
{
    if (slot < 0 || slot >= bot_controller::buy_controller_state::kMaxSlots) return -2;
    bot_controller::buy_controller_state::Clear(slot);
    return 0;
}

extern "C" BC_EXPORT int BotController_ClearAllBuyPlans()
{
    bot_controller::buy_controller_state::ClearAll();
    return 0;
}

// Item count for a slot's plan: -1 none, 0 skip/empty, >0 alias count.
extern "C" BC_EXPORT int BotController_GetBuyPlanItemCount(int slot) { return bot_controller::buy_controller_state::ItemCount(slot); }

// ---- Motion recording & replay ----

// Begin/stop recording a human slot's per-tick movement. 0 ok / -1 fail.
extern "C" BC_EXPORT int BotController_StartRecord(int slot) { return bot_controller::motion_recorder::StartRecord(slot) ? 0 : -1; }

extern "C" BC_EXPORT int BotController_StopRecord(int slot) { return bot_controller::motion_recorder::StopRecord(slot) ? 0 : -1; }

// Recorded tick / subtick counts for a slot. <0 on bad slot.
extern "C" BC_EXPORT int BotController_GetRecordedTickCount(int slot) { return bot_controller::motion_recorder::RecordedTickCount(slot); }

extern "C" BC_EXPORT int BotController_GetRecordedSubtickCount(int slot)
{
    return bot_controller::motion_recorder::RecordedSubtickCount(slot);
}

// Recorded command-frame count for a slot
extern "C" BC_EXPORT int BotController_GetRecordedCommandCount(int slot)
{
    return bot_controller::motion_recorder::RecordedCommandCount(slot);
}

// Copy recorded ticks / subticks into caller buffers. Returns count written.
extern "C" BC_EXPORT int BotController_CopyRecordedTicks(int slot, bot_controller::ReplayTick* out, int maxTicks)
{
    return bot_controller::motion_recorder::CopyTicks(slot, out, maxTicks);
}

extern "C" BC_EXPORT int BotController_CopyRecordedSubticks(int slot, bot_controller::SubtickMove* out, int maxSubticks)
{
    return bot_controller::motion_recorder::CopySubticks(slot, out, maxSubticks);
}

// Copies recorded command frames into a caller-owned buffer
extern "C" BC_EXPORT int BotController_CopyRecordedCommands(int slot, bot_controller::ReplayCommandFrameData* out, int maxCommands)
{
    return bot_controller::motion_recorder::CopyCommands(slot, out, maxCommands);
}

// Load parallel tick + subtick arrays into a slot's replay buffer. 0 ok.
extern "C" BC_EXPORT int BotController_LoadReplay(
    int slot, const bot_controller::ReplayTick* ticks, int tickCount, const bot_controller::SubtickMove* subs, int subCount) noexcept
{
    return bot_controller::motion_recorder::LoadReplay(slot, ticks, tickCount, subs, subCount) ? 0 : -1;
}

// Load replay buffers with optional per-tick command and movement data
extern "C" BC_EXPORT int BotController_LoadReplayExtended(int slot,
                                                          const bot_controller::ReplayTick* ticks,
                                                          int tickCount,
                                                          const bot_controller::SubtickMove* subs,
                                                          int subCount,
                                                          const bot_controller::ReplayCommandFrameData* commands,
                                                          int commandCount,
                                                          const bot_controller::ReplayMovementExtra* movementExtras,
                                                          int movementExtraCount) noexcept
{
    return bot_controller::motion_recorder::LoadReplayExtended(slot, ticks, tickCount, subs, subCount, commands, commandCount,
                                                               movementExtras, movementExtraCount)
               ? 0
               : -1;
}

// Move a slot's just-recorded buffers into another slot's replay buffer
extern "C" BC_EXPORT int BotController_TransferRecordingToReplay(int srcSlot, int dstSlot)
{
    int nt = bot_controller::motion_recorder::RecordedTickCount(srcSlot);
    if (nt <= 0) return -1;
    int ns = bot_controller::motion_recorder::RecordedSubtickCount(srcSlot);
    ns = std::max(ns, 0);
    int nc = bot_controller::motion_recorder::RecordedCommandCount(srcSlot);
    if (nc != nt) return -1;
    std::vector<bot_controller::ReplayTick> ticks(nt);
    std::vector<bot_controller::SubtickMove> subs(ns > 0 ? ns : 1);
    std::vector<bot_controller::ReplayCommandFrameData> commands(nc);
    int gotT = bot_controller::motion_recorder::CopyTicks(srcSlot, ticks.data(), nt);
    int gotS = ns > 0 ? bot_controller::motion_recorder::CopySubticks(srcSlot, subs.data(), ns) : 0;
    int gotC = bot_controller::motion_recorder::CopyCommands(srcSlot, commands.data(), nc);
    if (gotT <= 0 || gotC != gotT) return -1;
    return bot_controller::motion_recorder::LoadReplayExtended(dstSlot, ticks.data(), gotT, subs.data(), gotS, commands.data(), gotC,
                                                               nullptr, 0)
               ? 0
               : -1;
}

extern "C" BC_EXPORT int BotController_StartReplay(int slot, int loop)
{
    return bot_controller::motion_recorder::StartReplay(slot, loop != 0) ? 0 : -1;
}

// Registers the managed plugin's authoritative pawn pointer for replay.
extern "C" BC_EXPORT int BotController_SetReplayPawn(int slot, uint64_t pawnPtr)
{
    void* pawn = reinterpret_cast<void*>(static_cast<uintptr_t>(pawnPtr)); // NOLINT(performance-no-int-to-ptr)
    return bot_controller::input_injector::SetReplayPawn(slot, pawn) ? 0 : -1;
}

extern "C" BC_EXPORT int BotController_StopReplay(int slot) { return bot_controller::motion_recorder::StopReplay(slot) ? 0 : -1; }

// Current replay tick index, or <0 if the slot is not replaying.
extern "C" BC_EXPORT int BotController_GetReplayCursor(int slot) { return bot_controller::motion_recorder::ReplayCursor(slot); }

// Total ticks loaded in a slot's replay buffer.
extern "C" BC_EXPORT int BotController_GetReplayTotal(int slot) { return bot_controller::motion_recorder::ReplayTotal(slot); }

// Copy the tick currently being replayed (for C# to drive weapon/fire).
// Returns 0 on success, -1 if the slot isn't replaying.
extern "C" BC_EXPORT int BotController_GetReplayTick(int slot, bot_controller::ReplayTick* out)
{
    if (!out) return -1;
    return bot_controller::motion_recorder::CurrentReplayTick(slot, *out) ? 0 : -1;
}

// Switch a bot to the weapon with this def index
// Returns 0 ok / -1 not found or bot not ready.
extern "C" BC_EXPORT int BotController_SwitchBotWeapon(int slot, int defIndex)
{
    return bot_controller::motion_recorder::SwitchBotWeaponByDef(slot, defIndex) ? 0 : -1;
}

// Def index of the bot's current active weapon (same normalization as the
// recorded WeaponDefIndex). <0 if unresolved. For C# to reconcile replay.
extern "C" BC_EXPORT int BotController_GetBotActiveWeaponDef(int slot) { return bot_controller::motion_recorder::BotActiveWeaponDef(slot); }

extern "C" BC_EXPORT uint64_t BotController_GetHookCallCount() { return bot_controller::input_injector::HookCallCount(); }

extern "C" BC_EXPORT int BotController_GetLastResolvedSlot() { return bot_controller::input_injector::LastResolvedSlot(); }

extern "C" BC_EXPORT uint64_t BotController_GetFinishMoveCallCount() { return bot_controller::input_injector::FinishMoveCallCount(); }

extern "C" BC_EXPORT uint64_t BotController_GetPlayerRunCommandCallCount()
{
    return bot_controller::input_injector::PlayerRunCommandCallCount();
}

extern "C" BC_EXPORT uint64_t BotController_GetPhysicsSimulateCallCount()
{
    return bot_controller::input_injector::PhysicsSimulateCallCount();
}

extern "C" BC_EXPORT int BotController_GetLastPhysicsSlot() { return bot_controller::input_injector::LastPhysicsSlot(); }

extern "C" BC_EXPORT uint64_t BotController_GetReplayCommitCount() { return bot_controller::input_injector::ReplayCommitCount(); }

extern "C" BC_EXPORT uint64_t BotController_GetSlotResolveCallCount() { return bot_controller::input_injector::SlotResolveCallCount(); }

extern "C" BC_EXPORT uint64_t BotController_GetSlotResolveFailureCount()
{
    return bot_controller::input_injector::SlotResolveFailureCount();
}

extern "C" BC_EXPORT uint64_t BotController_GetLastServices()
{
    return static_cast<uint64_t>(bot_controller::input_injector::LastServices());
}

extern "C" BC_EXPORT uint64_t BotController_GetLastPawn() { return static_cast<uint64_t>(bot_controller::input_injector::LastPawn()); }

extern "C" BC_EXPORT uint32_t BotController_GetLastControllerHandle() { return bot_controller::input_injector::LastControllerHandle(); }

extern "C" BC_EXPORT uint32_t BotController_GetLastOriginalControllerHandle()
{
    return bot_controller::input_injector::LastOriginalControllerHandle();
}

extern "C" BC_EXPORT int BotController_GetLastControllerIndex() { return bot_controller::input_injector::LastControllerIndex(); }

extern "C" BC_EXPORT int BotController_GetLastOriginalControllerIndex()
{
    return bot_controller::input_injector::LastOriginalControllerIndex();
}

extern "C" BC_EXPORT int BotController_GetLastOwnerSlot() { return bot_controller::input_injector::LastOwnerSlot(); }

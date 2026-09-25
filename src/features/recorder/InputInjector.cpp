#include "core/gameconfig.h"
#include "core/log.h"
// CS2 movement hooks
// PhysicsSimulate (record/replay frame boundary)
// ProcessMovement (live services discovery)
// PlayerRunCommand(subtick record + re-inject)

#include "networkbasetypes.pb.h"
#include "nlohmann/json.hpp" // NOLINT(misc-include-cleaner)
#include "playercommand.h"

#include "InputInjector.h"
#include "PawnBinding.h"
#include "ccsbot_slot.h"
#include "core/memory_module.h"
#include "MotionRecorder.h"
#include "Gate2BDiagnostics.h"
#include "Gate2CDiagnostics.h"
#include "BotControllerState.h"
#include "usercmd.pb.h"
#include "offsets.h"
#include "hooks.h"
#include "WeaponLocker.h"

#include <algorithm> // NOLINT(misc-include-cleaner)
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector> // NOLINT(misc-include-cleaner)

#include <tier0/dbg.h>

namespace tg = cs2bc::offsets;

namespace cs2bc {
namespace input_injector {

namespace {
constexpr float kUsercmdKeyboardMoveScale = 450.0F;
constexpr uint64_t kInForward = 1ULL << 3;
constexpr uint64_t kInBack = 1ULL << 4;
constexpr uint64_t kInMoveLeft = 1ULL << 9;
constexpr uint64_t kInMoveRight = 1ULL << 10;
constexpr uint64_t kMovementButtonMask = kInForward | kInBack | kInMoveLeft | kInMoveRight;

void* g_addrProcessMovement = nullptr;
void* g_addrPlayerRunCommand = nullptr;
void* g_addrPhysicsSimulate = nullptr;

hooks::NativeHook<void, void*, void*> g_hookProcessMovement;
hooks::NativeHook<void, void*, void*> g_hookPlayerRunCommand;
hooks::NativeHook<void, void*> g_hookPhysicsSimulate;
hooks::NativeHook<void, void*> g_hookControllerCommandSetup;
std::atomic<bool> g_controllerHookTried{ false };

struct MovementFrame
{
    int slot;
    void* services;
    bool recording;
    bool replaying;
    bool seeded = false;
};
thread_local std::vector<MovementFrame> g_physicsFrames;

// Gate 2C: one pending ProcessMovement invocation's PRE+INJECTED data,
// pushed by the PRE hook and matched+popped by the POST hook. thread_local,
// mirroring g_physicsFrames above -- but unlike g_physicsFrames, POST does
// NOT trust stack position alone: it verifies (slot, services, moveData)
// identity before treating a frame as its match, and every frame carries
// the diagnostic generation it was created under so a stale frame can
// never be recorded into a newer test (see Gate2CDiagnostics.h).
//
// hasData is why this is a MARKER stack, not just a data stack: a PRE call
// that finds Gate2C inactive (or admission otherwise not open) still
// pushes an entry -- with hasData=false and no diagnostic fields filled in
// -- whenever a real, still-pending frame might exist beneath it on this
// thread (see the push condition in HookedProcessMovement). Without this,
// a nested/reentrant call sharing an OLDER frame's exact identity
// (slot+services+moveData -- plausible if CMoveData is a reused per-tick
// buffer, not just under true call nesting) would push nothing, and its
// POST -- which does not gate on IsActiveForSlot -- would have no way to
// tell "no frame was ever meant for me" apart from "my frame is just
// deeper in the stack," and could wrongly match and consume the older
// call's real frame. A hasData=false marker reserves this call's stack
// position so POST always finds SOMETHING for it (real data or an
// explicit non-recording marker) before it could ever fall through to an
// unrelated older frame.
//
// Depth is bounded (kGate2CMaxFrameDepth). An EARLIER design evicted the
// single oldest pending entry to make room for a new push. That is NOT
// safe: the evicted invocation's real ProcessMovement call may still be
// executing (genuine deep nesting), and if a LATER, unrelated invocation
// with the SAME (slot, services, moveData) identity is pushed afterward,
// the evicted call's eventual POST would search the stack, find that
// later unrelated frame as an apparent match, and consume it -- silently
// pairing two different invocations. Eviction only guarantees "no match
// found is safe"; it does not guarantee "a match found is the right one"
// once an identity has been allowed to reappear after a gap.
//
// Instead: once depth would be exceeded, this thread's Gate2C tracking is
// POISONED for the (slot, generation) pair in effect at that moment (see
// g_gate2cPoisoned below). Poisoning wipes every currently-pending frame,
// stops all further CMoveData writes and frame pushes for that (slot,
// generation) on this thread, and marks the diagnostic result INVALID
// (not merely "some samples dropped") once Finalize runs -- see
// RecordOverflowPoisoned in Gate2CDiagnostics.h. A fresh Start() (a new
// generation) clears the poison automatically, since it is checked
// against the CURRENT generation, not a persistent flag that outlives it.
struct Gate2CFrame
{
    int slot = -1;
    void* services = nullptr;
    void* moveData = nullptr;
    uint64_t invocationId = 0;
    uint32_t generation = 0;
    bool hasData = false; // false: non-recording marker only -- every field
                           // below is meaningless and MUST NOT be read.
    float preForward = 0, preSide = 0;
    float injectedForward = 0, injectedSide = 0;
    bool wroteIntent = false;
    bool lockAllActive = false;
};
constexpr size_t kGate2CMaxFrameDepth = 8;
thread_local std::vector<Gate2CFrame> g_gate2cFrames;

// Set once this thread's Gate2C tracking has been poisoned by depth
// overflow (see the comment above Gate2CFrame). Scoped to the specific
// (slot, generation) pair active at the moment of poisoning: a different
// slot, or the same slot after a fresh Start() advances the generation,
// is NOT considered poisoned -- there is nothing stale left to mismatch
// against either way, since poisoning always wipes g_gate2cFrames.
thread_local bool g_gate2cPoisoned = false;
thread_local int g_gate2cPoisonedSlot = -1;
thread_local uint32_t g_gate2cPoisonedGeneration = 0;

// True if this thread's Gate2C tracking is currently poisoned FOR slot,
// i.e. for the exact (slot, generation) pair that triggered poisoning.
bool Gate2CThreadPoisonedFor(int slot)
{
    if (!g_gate2cPoisoned) return false;
    if (slot != g_gate2cPoisonedSlot) return false;
    return gate2c::CurrentGeneration(slot) == g_gate2cPoisonedGeneration;
}

bool g_installed = false;
// True once PhysicsSimulate is hooked
bool g_physicsActive = false;
// True once PlayerRunCommand is hooked
bool g_subtickActive = false;
std::string g_status = "not_attempted"; // NOLINT(bugprone-throwing-static-initialization)

// slot -> live CCSPlayer_MovementServices*
std::array<std::atomic<void*>, kMaxSlots> g_slotServices{};

enum class UsercmdInjectionPhase : uint8_t
{
    PendingPress,
    Holding,
    PendingRelease
};

struct UsercmdInjection
{
    int64_t id;
    uint64_t buttonMask;
    int64_t expiresAtMs;
    int durationMs;
    UsercmdInjectionPhase phase;
};

struct UsercmdMovement
{
    int64_t id;
    float forwardMove;
    float leftMove;
};

struct UsercmdSuppression
{
    int64_t id;
    uint64_t buttonMask;
    int64_t expiresAtMs;
    bool persistent;
    bool releasePending;
};

std::array<std::vector<UsercmdInjection>, kMaxSlots> g_usercmdInjections{};
std::array<std::vector<UsercmdSuppression>, kMaxSlots> g_usercmdSuppressions{};
std::array<std::vector<UsercmdMovement>, kMaxSlots> g_usercmdMovements{};
std::array<uint64_t, kMaxSlots> g_injectedHeldMasks{};
std::array<uint64_t, kMaxSlots> g_movementHeldMasks{};
std::mutex g_usercmdInjectionMutex;
std::atomic<int64_t> g_nextUsercmdInjectionId{ 1 };
std::atomic<int64_t> g_nextUsercmdSuppressionId{ 1 };
std::atomic<int64_t> g_nextUsercmdMovementId{ 1 };

bool IsThrowableUtilityDef(int def) { return def >= 43 && def <= 48; }

// Reports whether a slot can index the fixed replay state arrays.
bool ValidSlotIndex(int slot) { return slot >= 0 && slot < kMaxSlots; }

float NormalizeDeg(float a)
{
    a = std::fmod(a + 180.0F, 360.0F);
    if (a < 0.0F) a += 360.0F;
    return a - 180.0F;
}

// Returns monotonic time in milliseconds for injection expiry checks
int64_t MonotonicMilliseconds()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// Creates an independently cancellable usercmd button injection
} // namespace

int64_t InjectUsercmd(int slot, uint64_t buttonMask, int durationMs)
{
    if (!ValidSlotIndex(slot) || buttonMask == 0 || durationMs < 0 || !g_subtickActive || motion_recorder::IsReplaying(slot)) return -1;

    int64_t id = g_nextUsercmdInjectionId.fetch_add(1, std::memory_order_relaxed);
    std::scoped_lock lock(g_usercmdInjectionMutex);
    if (motion_recorder::IsReplaying(slot)) return -1;
    g_usercmdInjections[slot].push_back(
        { .id = id, .buttonMask = buttonMask, .expiresAtMs = 0, .durationMs = durationMs, .phase = UsercmdInjectionPhase::PendingPress });
    return id;
}

// Creates an independently cancellable persistent analog movement override
int64_t StartUsercmdMovement(int slot, float forwardMove, float leftMove)
{
    if (!ValidSlotIndex(slot) || !std::isfinite(forwardMove) || !std::isfinite(leftMove) || !g_subtickActive ||
        motion_recorder::IsReplaying(slot))
        return -1;

    int64_t id = g_nextUsercmdMovementId.fetch_add(1, std::memory_order_relaxed);
    std::scoped_lock lock(g_usercmdInjectionMutex);
    if (motion_recorder::IsReplaying(slot)) return -1;
    g_usercmdMovements[slot].push_back(
        { .id = id, .forwardMove = std::clamp(forwardMove, -1.0F, 1.0F), .leftMove = std::clamp(leftMove, -1.0F, 1.0F) });
    return id;
}

// Updates one persistent analog movement override
bool UpdateUsercmdMovement(int slot, int64_t movementId, float forwardMove, float leftMove)
{
    if (!ValidSlotIndex(slot) || movementId <= 0 || !std::isfinite(forwardMove) || !std::isfinite(leftMove)) return false;

    std::scoped_lock lock(g_usercmdInjectionMutex);
    for (UsercmdMovement& movement : g_usercmdMovements[slot])
    {
        if (movement.id != movementId) continue;
        movement.forwardMove = std::clamp(forwardMove, -1.0F, 1.0F);
        movement.leftMove = std::clamp(leftMove, -1.0F, 1.0F);
        return true;
    }
    return false;
}

// Cancels one persistent analog movement override
bool CancelUsercmdMovement(int slot, int64_t movementId)
{
    if (!ValidSlotIndex(slot) || movementId <= 0) return false;

    std::scoped_lock lock(g_usercmdInjectionMutex);
    auto& movements = g_usercmdMovements[slot];
    for (auto it = movements.begin(); it != movements.end(); ++it)
    {
        if (it->id != movementId) continue;
        movements.erase(it);
        return true;
    }
    return false;
}

// Cancels one injection without affecting other active tokens
bool CancelUsercmdInjection(int slot, int64_t injectionId)
{
    if (!ValidSlotIndex(slot) || injectionId <= 0) return false;

    std::scoped_lock lock(g_usercmdInjectionMutex);
    auto& injections = g_usercmdInjections[slot];
    for (auto it = injections.begin(); it != injections.end(); ++it)
    {
        if (it->id != injectionId) continue;

        if (it->phase == UsercmdInjectionPhase::PendingPress) injections.erase(it);
        else
            it->phase = UsercmdInjectionPhase::PendingRelease;
        return true;
    }
    return false;
}

// Suppresses selected usercmd buttons until the requested duration expires
bool SuppressUsercmd(int slot, uint64_t buttonMask, int durationMs)
{
    if (!ValidSlotIndex(slot) || buttonMask == 0 || durationMs <= 0 || !g_subtickActive || motion_recorder::IsReplaying(slot)) return false;

    int64_t expiresAtMs = MonotonicMilliseconds() + durationMs;
    std::scoped_lock lock(g_usercmdInjectionMutex);
    if (motion_recorder::IsReplaying(slot)) return false;
    g_usercmdSuppressions[slot].push_back(
        { .id = 0, .buttonMask = buttonMask, .expiresAtMs = expiresAtMs, .persistent = false, .releasePending = true });
    return true;
}

// Creates an independently cancellable persistent usercmd suppression
int64_t StartUsercmdSuppression(int slot, uint64_t buttonMask)
{
    if (!ValidSlotIndex(slot) || buttonMask == 0 || !g_subtickActive || motion_recorder::IsReplaying(slot)) return -1;

    int64_t id = g_nextUsercmdSuppressionId.fetch_add(1, std::memory_order_relaxed);
    std::scoped_lock lock(g_usercmdInjectionMutex);
    if (motion_recorder::IsReplaying(slot)) return -1;
    g_usercmdSuppressions[slot].push_back(
        { .id = id, .buttonMask = buttonMask, .expiresAtMs = 0, .persistent = true, .releasePending = true });
    return id;
}

// Cancels one persistent usercmd suppression by its token
bool CancelUsercmdSuppression(int slot, int64_t suppressionId)
{
    if (!ValidSlotIndex(slot) || suppressionId <= 0) return false;

    std::scoped_lock lock(g_usercmdInjectionMutex);
    auto& suppressions = g_usercmdSuppressions[slot];
    for (auto it = suppressions.begin(); it != suppressions.end(); ++it)
    {
        if (it->id != suppressionId) continue;
        suppressions.erase(it);
        return true;
    }
    return false;
}

// Removes every injection and suppression so replay starts without deferred input
void ClearUsercmdInjections(int slot)
{
    if (!ValidSlotIndex(slot)) return;

    std::scoped_lock lock(g_usercmdInjectionMutex);
    g_usercmdInjections[slot].clear();
    g_usercmdSuppressions[slot].clear();
    g_usercmdMovements[slot].clear();
    g_injectedHeldMasks[slot] = 0;
    g_movementHeldMasks[slot] = 0;
}

// Queries pending input ownership without changing press/release state.
namespace {

struct UsercmdWork
{
    bool injection;
    bool suppression;
    bool movement;
};

// Takes one consistent snapshot instead of locking once per input kind.
UsercmdWork GetUsercmdWork(int slot)
{
    if (!ValidSlotIndex(slot)) return {};

    std::scoped_lock lock(g_usercmdInjectionMutex);
    return { !g_usercmdInjections[slot].empty(), !g_usercmdSuppressions[slot].empty(), !g_usercmdMovements[slot].empty() };
}

// Gate 2C: read-only peek at the SAME authoritative movement intent
// ApplyUsercmdMovement (the existing PlayerRunCommand path) already reads
// -- same array, same mutex, same "most recently started movement wins"
// (.back()) semantics, same replaying exclusion. Does not create a second,
// competing store of movement state: this is the only other reader of
// g_usercmdMovements, and it never mutates it.
bool PeekUsercmdMovement(int slot, float& forwardOut, float& leftOut)
{
    if (!ValidSlotIndex(slot) || motion_recorder::IsReplaying(slot)) return false;
    std::scoped_lock lock(g_usercmdInjectionMutex);
    if (g_usercmdMovements[slot].empty()) return false;
    const UsercmdMovement& m = g_usercmdMovements[slot].back();
    forwardOut = m.forwardMove;
    leftOut = m.leftMove;
    return true;
}

// Replaces Bot AI analog movement after the final command is generated
bool ApplyUsercmdMovement(int slot, PlayerCommand* pc, CBaseUserCmdPB* base) // NOLINT(readability-non-const-parameter)
{
    if (!ValidSlotIndex(slot) || pc == nullptr || base == nullptr) return false;

    UsercmdMovement movement{};
    uint64_t previousMask = 0;
    {
        std::scoped_lock lock(g_usercmdInjectionMutex);
        const auto& movements = g_usercmdMovements[slot];
        if (movements.empty()) return false;
        movement = movements.back();
        previousMask = g_movementHeldMasks[slot];
    }

    uint64_t movementMask = 0;
    if (movement.forwardMove > 0.0F) movementMask |= kInForward;
    else if (movement.forwardMove < 0.0F)
        movementMask |= kInBack;
    if (movement.leftMove > 0.0F) movementMask |= kInMoveLeft;
    else if (movement.leftMove < 0.0F)
        movementMask |= kInMoveRight;

    {
        std::scoped_lock lock(g_usercmdInjectionMutex);
        g_movementHeldMasks[slot] = movementMask;
    }

    uint64_t pressedMask = movementMask & ~previousMask;
    uint64_t releasedMask = previousMask & ~movementMask;
    uint64_t held = (pc->buttonstates.m_pButtonStates[0] & ~kMovementButtonMask) | movementMask;
    uint64_t pressed = (pc->buttonstates.m_pButtonStates[1] & ~kMovementButtonMask) | pressedMask;
    uint64_t released = (pc->buttonstates.m_pButtonStates[2] & ~movementMask) | releasedMask;

    CInButtonStatePB* buttons = base->mutable_buttons_pb();
    buttons->set_buttonstate1(held);
    buttons->set_buttonstate2(pressed);
    buttons->set_buttonstate3(released);
    pc->buttonstates.m_pButtonStates[0] = held;
    pc->buttonstates.m_pButtonStates[1] = pressed;
    pc->buttonstates.m_pButtonStates[2] = released;

    base->set_forwardmove(movement.forwardMove * kUsercmdKeyboardMoveScale);
    base->set_leftmove(movement.leftMove * kUsercmdKeyboardMoveScale);
    for (int index = 0; index < base->subtick_moves_size(); ++index)
    {
        CSubtickMoveStep* step = base->mutable_subtick_moves(index);
        step->set_analog_forward_delta(0.0F);
        step->set_analog_left_delta(0.0F);
    }
    return true;
}

// Merges active injections and emits aggregate press and release edges
bool ApplyUsercmdInjections(int slot, PlayerCommand* pc, CBaseUserCmdPB* base)
{
    if (!ValidSlotIndex(slot) || !pc || !base) return false;

    uint64_t activeMask = 0;
    uint64_t previousMask = 0;
    {
        std::scoped_lock lock(g_usercmdInjectionMutex);
        auto& injections = g_usercmdInjections[slot];
        int64_t nowMs = MonotonicMilliseconds();
        for (auto it = injections.begin(); it != injections.end();)
        {
            UsercmdInjectionPhase phase = it->phase;
            if (phase == UsercmdInjectionPhase::PendingRelease || (phase == UsercmdInjectionPhase::Holding && nowMs >= it->expiresAtMs))
            {
                it = injections.erase(it);
                continue;
            }

            activeMask |= it->buttonMask;
            if (phase == UsercmdInjectionPhase::PendingPress)
            {
                if (it->durationMs > 0) it->expiresAtMs = nowMs + it->durationMs;
                it->phase = it->durationMs == 0 ? UsercmdInjectionPhase::PendingRelease : UsercmdInjectionPhase::Holding;
            }
            ++it;
        }

        previousMask = g_injectedHeldMasks[slot];
        g_injectedHeldMasks[slot] = activeMask;
    }

    uint64_t pressedMask = activeMask & ~previousMask;
    uint64_t releasedMask = previousMask & ~activeMask;
    uint64_t held = pc->buttonstates.m_pButtonStates[0];
    uint64_t pressed = pc->buttonstates.m_pButtonStates[1];
    uint64_t released = pc->buttonstates.m_pButtonStates[2];

    held = (held & ~releasedMask) | activeMask;
    pressed = (pressed & ~releasedMask) | pressedMask;
    released = (released & ~pressedMask) | releasedMask;

    CInButtonStatePB* buttons = base->mutable_buttons_pb();
    buttons->set_buttonstate1(held);
    buttons->set_buttonstate2(pressed);
    buttons->set_buttonstate3(released);
    pc->buttonstates.m_pButtonStates[0] = held;
    pc->buttonstates.m_pButtonStates[1] = pressed;
    pc->buttonstates.m_pButtonStates[2] = released;
    return true;
}

// Removes suppressed buttons after Bot AI has produced the final command
bool ApplyUsercmdSuppressions(int slot, PlayerCommand* pc, CBaseUserCmdPB* base) // NOLINT(readability-non-const-parameter)
{
    if (!ValidSlotIndex(slot) || !pc || !base) return false;

    uint64_t suppressedMask = 0;
    uint64_t releasedMask = 0;
    {
        std::scoped_lock lock(g_usercmdInjectionMutex);
        auto& suppressions = g_usercmdSuppressions[slot];
        int64_t nowMs = MonotonicMilliseconds();
        for (auto it = suppressions.begin(); it != suppressions.end();)
        {
            if (!it->persistent && nowMs >= it->expiresAtMs)
            {
                it = suppressions.erase(it);
                continue;
            }

            suppressedMask |= it->buttonMask;
            if (it->releasePending)
            {
                releasedMask |= it->buttonMask;
                it->releasePending = false;
            }
            ++it;
        }
    }

    if (suppressedMask == 0) return false;

    uint64_t held = pc->buttonstates.m_pButtonStates[0] & ~suppressedMask;
    uint64_t pressed = pc->buttonstates.m_pButtonStates[1] & ~suppressedMask;
    uint64_t released = pc->buttonstates.m_pButtonStates[2] | releasedMask;

    CInButtonStatePB* buttons = base->mutable_buttons_pb();
    buttons->set_buttonstate1(held);
    buttons->set_buttonstate2(pressed);
    buttons->set_buttonstate3(released);
    pc->buttonstates.m_pButtonStates[0] = held;
    pc->buttonstates.m_pButtonStates[1] = pressed;
    pc->buttonstates.m_pButtonStates[2] = released;

    for (int i = 0; i < base->subtick_moves_size(); ++i)
    {
        CSubtickMoveStep* move = base->mutable_subtick_moves(i);
        uint64_t buttonsMask = move->button() & ~suppressedMask;
        move->set_button(buttonsMask);
        if (buttonsMask == 0) move->set_pressed(false);
    }
    return true;
}

// Applies one recorded G-drop event through the bot client-command path
void ApplyReplayDrop(int slot, void* services)
{
    ReplayDropEvent event{};
    if (!motion_recorder::TakeCurrentReplayDrop(slot, event)) return;
    motion_recorder::DropReplayEventWeapon(slot, services, event);
}

// Installs the command hook from the live movement-services vtable.
void EnsureVtableHooks(void* services);

// Returns the enclosing simulation boundary for this player.
MovementFrame* FindPhysicsFrame(int slot);

// Discovers live services without modifying any substep simulation state.
// Gate 2B diagnostic addition: the second parameter is CMoveData* -- its
// identity was independently verified this session via its own +0x2C/+0x30/
// +0xC8 data flow (the engine's own "forward %f" debug string and the
// wishdir basis-vector math), not merely inferred from argument position.
// This remains a KHook pre-hook (per hooks.h's Install(target, pre, post)
// naming and KHook's own documented Ignore/Supercede semantics): it runs
// before the original ProcessMovement body, and returning Ignore here does
// not by itself prove the original subsequently executed -- KHook is a
// centralized, multi-consumer detouring system, and a PRE callback firing is
// not documented as 1:1 with the original's execution. The counters added
// below are therefore labeled "this hook was reached," never "the original
// ran." Only reads are added; nothing here writes CMoveData.
KHook::Return<void> HookedProcessMovement(void* services, void* moveData) noexcept
{
    EnsureVtableHooks(services);
    void* validatedPawn = nullptr;
    int slot = pawn_binding::ServicesToSlot(services, &validatedPawn);
    gate2b::RecordProcessMovementEntry(slot); // unconditional; safe even if slot<0

    if (slot >= 0 && slot < kMaxSlots)
    {
        g_slotServices[slot].store(services, std::memory_order_release);
        if (motion_recorder::IsRecording(slot))
            motion_recorder::SetLiveWs(slot, pawn_binding::ServicesToWeaponServices(slot, services, validatedPawn));

        if (moveData != nullptr)
        {
            const auto* base = static_cast<const uint8_t*>(moveData);
            float forwardMove, sideMove, velX, velY, velZ;
            std::memcpy(&forwardMove, base + 0x2C, sizeof(float));
            std::memcpy(&sideMove, base + 0x30, sizeof(float));
            std::memcpy(&velX, base + 0x38, sizeof(float));
            std::memcpy(&velY, base + 0x3C, sizeof(float));
            std::memcpy(&velZ, base + 0x40, sizeof(float));

            gate2b::RecordProcessMovementObservation(slot, gate2b::CurrentPhysicsSimulateSeq(slot),
                                                       MonotonicMilliseconds(), forwardMove, sideMove, velX, velY,
                                                       velZ);

            // ---- Gate 2C: diagnostic-only, gated write. Inert unless an
            // explicit Gate2CDiagnostics::Start(slot, ...) is active for
            // this slot -- see Gate2CDiagnostics.h. Writes only the
            // independently verified ForwardMove(+0x2C)/SideMove(+0x30)
            // fields (same offsets Gate2B already reads above); UpMove
            // and any services-level button state are NOT written --
            // neither has a verified offset/mechanism. Runs BEFORE the
            // original ProcessMovement body (this hook remains Ignore),
            // so the real physics step consumes whatever is written here.
            //
            // Pairing: pushes a Gate2CFrame carrying this call's identity
            // (slot/services/moveData), a unique invocation id, and the
            // slot's CURRENT diagnostic generation. The POST hook matches
            // on identity, not stack position alone -- see Gate2CFrame's
            // declaration for why (KHook does not document pre/post as
            // immune to reentrancy).
            //
            // A marker (real or non-recording) is pushed whenever Gate2C is
            // active OR this thread's stack is already non-empty -- the
            // latter covers a call that finds Gate2C inactive while an
            // OLDER, still-pending real frame with the same identity might
            // exist beneath it (see Gate2CFrame's comment for why that's a
            // real hazard, not a hypothetical one). When neither condition
            // holds (the overwhelmingly common case once Gate2C has never
            // been touched), this is a single empty-check, no allocation.
            const bool poisonedForThisCall = Gate2CThreadPoisonedFor(slot);
            if (!poisonedForThisCall)
            {
                const bool gate2cActive = gate2c::IsActiveForSlot(slot);
                if (gate2cActive || !g_gate2cFrames.empty())
                {
                    if (g_gate2cFrames.size() >= kGate2CMaxFrameDepth)
                    {
                        // Ambiguous-pairing risk past this point (see the
                        // comment above Gate2CFrame's declaration): poison
                        // this thread for this (slot, generation) rather
                        // than evict-and-continue. Wipe every pending
                        // frame, stop injecting and stop collecting for the
                        // rest of this generation, and mark the diagnostic
                        // result INVALID once Finalize runs. A fresh
                        // Start() (a new generation) clears this
                        // automatically via Gate2CThreadPoisonedFor's
                        // generation check.
                        g_gate2cPoisoned = true;
                        g_gate2cPoisonedSlot = slot;
                        g_gate2cPoisonedGeneration = gate2c::CurrentGeneration(slot);
                        g_gate2cFrames.clear();
                        gate2c::RecordOverflowPoisoned(slot);
                        // No push, no CMoveData write, for this invocation
                        // either -- it is the one that triggered the
                        // ambiguity; the safe response is to stop tracking
                        // entirely, not to salvage a partial record of it.
                    }
                    else
                    {
                        Gate2CFrame frame;
                        frame.slot = slot;
                        frame.services = services;
                        frame.moveData = moveData;
                        frame.invocationId = gate2c::NextInvocationId();
                        frame.hasData = gate2cActive;

                        if (gate2cActive)
                        {
                            float wantForward = 0.0F, wantLeft = 0.0F;
                            bool wroteIntent = PeekUsercmdMovement(slot, wantForward, wantLeft);
                            float injectedForward = forwardMove, injectedSide = sideMove; // unless written below
                            const float scale = gate2c::WriteScaleForSlot(slot);
                            if (wroteIntent)
                            {
                                // writeScale is an explicit, test-supplied parameter
                                // (see Gate2CDiagnostics::Start) -- NOT assumed to be
                                // kUsercmdKeyboardMoveScale (450), and 1.0 is not
                                // asserted as the established CMoveData unit either.
                                // Gate2B's own runtime evidence observed
                                // CMoveData.forwardmove ~= 1.0 during a real,
                                // naturally-occurring forward tick, which is why 450
                                // must not be assumed correct here merely because the
                                // PlayerRunCommand path uses it for the usercmd's
                                // legacy scalar field -- a different field, on a
                                // different object, with no independently verified
                                // shared unit. Both remain hypotheses the test itself
                                // evaluates, not established facts.
                                float writeForward = wantForward * scale;
                                float writeSide = wantLeft * scale;
                                auto* mutableBase = static_cast<uint8_t*>(moveData);
                                std::memcpy(mutableBase + 0x2C, &writeForward, sizeof(float));
                                std::memcpy(mutableBase + 0x30, &writeSide, sizeof(float));
                                std::memcpy(&injectedForward, mutableBase + 0x2C, sizeof(float)); // read-back
                                std::memcpy(&injectedSide, mutableBase + 0x30, sizeof(float));
                            }

                            frame.generation = gate2c::CurrentGeneration(slot);
                            frame.preForward = forwardMove;
                            frame.preSide = sideMove;
                            frame.injectedForward = injectedForward;
                            frame.injectedSide = injectedSide;
                            frame.wroteIntent = wroteIntent;
                            frame.lockAllActive = bot_controller_state::GetAll(slot);
                        }
                        // else: hasData stays false. This is a non-recording
                        // marker only -- Gate2C is inactive for this call, but
                        // the marker still reserves this stack position so a
                        // later POST for THIS specific invocation cannot fall
                        // through to an older, unrelated real frame beneath it.

                        g_gate2cFrames.push_back(frame);
                    }
                }
            }
            // else: this thread is poisoned for this exact (slot, generation)
            // -- fully inert, no push, no write, until a fresh Start() for
            // this slot advances the generation.
        }
    }
    return { KHook::Action::Ignore };
}

// Gate 2C POST hook: reads CMoveData again AFTER the original
// ProcessMovement body has run (this is what Gate2B could not observe --
// it only ever had a pre-hook here). Read-only with respect to CMoveData:
// writes nothing. Registered as the `post` callback alongside
// HookedProcessMovement below -- the exact same pre+post pairing pattern
// already proven in this file for HookedPhysicsSimulate/PhysicsSimulatePost.
//
// Deliberately does NOT gate on gate2c::IsActiveForSlot: a frame pushed
// by the PRE hook just before Finalize() ran must still be found and
// popped here, or it would leak in this thread's stack forever (Finalize
// cannot reach into another thread's thread_local storage to clear it).
// The common Gate2C-disabled case is instead handled by the cheap
// g_gate2cFrames.empty() check immediately below -- no frame is ever
// pushed while inactive, so that array stays empty and this is a single
// vector-empty check, not meaningfully different in cost from the
// previous flag check.
KHook::Return<void> HookedProcessMovementPost(void* services, void* moveData) noexcept
{
    if (g_gate2cFrames.empty()) return { KHook::Action::Ignore };

    int slot = pawn_binding::ServicesToSlot(services);

    // Search from the top (most recently pushed) down for a frame whose
    // identity matches this exact call. Bounded by kGate2CMaxFrameDepth --
    // never an unbounded scan. A match found below the top means the
    // frame(s) above it belong to invocations whose own POST never
    // arrived (or arrived out of order) before this one did; those are
    // orphaned and discarded, never merged into any recorded sample.
    int matchIndex = -1;
    for (int i = static_cast<int>(g_gate2cFrames.size()) - 1; i >= 0; --i)
    {
        const Gate2CFrame& f = g_gate2cFrames[i];
        if (f.slot == slot && f.services == services && f.moveData == moveData)
        {
            matchIndex = i;
            break;
        }
    }

    if (matchIndex < 0)
    {
        gate2c::RecordUnmatchedPost(slot);
        return { KHook::Action::Ignore };
    }

    const int orphaned = static_cast<int>(g_gate2cFrames.size()) - 1 - matchIndex;
    for (int i = 0; i < orphaned; ++i) gate2c::RecordOrphanedFrame(slot);

    const Gate2CFrame frame = g_gate2cFrames[matchIndex];
    g_gate2cFrames.resize(static_cast<size_t>(matchIndex)); // drops the match and every orphan above it

    if (!frame.hasData)
    {
        // Non-recording marker: this invocation's PRE ran while Gate2C was
        // inactive (e.g. a test finalized between a nested call's PRE and
        // POST). Correctly matched -- proving no mismatch occurred -- but
        // there is no real PRE/INJECTED data to record, and none of this
        // frame's other fields are meaningful. Counted distinctly so this
        // expected case is never confused with an unmatched/orphaned one.
        gate2c::RecordNonRecordingMatch(slot);
        return { KHook::Action::Ignore };
    }

    const auto* base = static_cast<const uint8_t*>(moveData);
    float postForward, postSide, velX, velY, velZ, originX, originY, originZ;
    std::memcpy(&postForward, base + 0x2C, sizeof(float));
    std::memcpy(&postSide, base + 0x30, sizeof(float));
    std::memcpy(&velX, base + 0x38, sizeof(float));
    std::memcpy(&velY, base + 0x3C, sizeof(float));
    std::memcpy(&velZ, base + 0x40, sizeof(float));
    std::memcpy(&originX, base + 0xC8, sizeof(float));
    std::memcpy(&originY, base + 0xCC, sizeof(float));
    std::memcpy(&originZ, base + 0xD0, sizeof(float));

    gate2c::RecordResult(slot, frame.generation, frame.invocationId, MonotonicMilliseconds(), frame.preForward,
                          frame.preSide, frame.wroteIntent, frame.injectedForward, frame.injectedSide,
                          frame.lockAllActive, postForward, postSide, velX, velY, velZ, originX, originY, originZ);
    return { KHook::Action::Ignore };
}

MovementFrame* FindPhysicsFrame(int slot)
{
    if (slot < 0 || slot >= kMaxSlots) return nullptr;
    for (auto it = g_physicsFrames.rbegin(); it != g_physicsFrames.rend(); ++it)
        if (it->slot == slot) return &*it;
    return nullptr;
}

// ---- PlayerRunCommand: subtick record + re-inject ----

// Serializes command fields and subticks into the pending recording frame.
void CaptureUserCommand(int slot, void* services, PlayerCommand* pc, CBaseUserCmdPB* base)
{
    // Read this tick's subtick_moves into SubtickMove[] and
    // stash; OnCapturePost (PhysicsSimulate-post) commits them.
    int n = base->subtick_moves_size();
    n = std::min(n, motion_recorder::kMaxSubtickPerTick);
    SubtickMove moves[motion_recorder::kMaxSubtickPerTick];
    for (int i = 0; i < n; ++i)
    {
        const CSubtickMoveStep& s = base->subtick_moves(i);
        moves[i].when = s.when();
        moves[i].button = static_cast<uint32_t>(s.button());
        moves[i].pressed = s.pressed() ? 1.0F : 0.0F;
        moves[i].analogForward = s.analog_forward_delta();
        moves[i].analogLeft = s.analog_left_delta();
        moves[i].pitchDelta = s.pitch_delta();
        moves[i].yawDelta = s.yaw_delta();
    }
    motion_recorder::OnCaptureSubticks(slot, moves, n);

    ReplayCommandFrameData command{};
    command.fields |= motion_recorder::kCommandFieldWeaponSelectDef;
    command.buttons = pc->buttonstates.m_pButtonStates[0];
    command.buttons1 = pc->buttonstates.m_pButtonStates[1];
    command.buttons2 = pc->buttonstates.m_pButtonStates[2];
    command.fields |= motion_recorder::kCommandFieldButtons;
    if (base->has_forwardmove())
    {
        command.forwardMove = base->forwardmove();
        command.fields |= motion_recorder::kCommandFieldForwardMove;
    }
    if (base->has_leftmove())
    {
        command.leftMove = base->leftmove();
        command.fields |= motion_recorder::kCommandFieldLeftMove;
    }
    if (base->has_upmove())
    {
        command.upMove = base->upmove();
        command.fields |= motion_recorder::kCommandFieldUpMove;
    }
    if (base->has_viewangles())
    {
        const CMsgQAngle& view = base->viewangles();
        command.pitch = view.x();
        command.yaw = view.y();
        command.roll = view.z();
        command.fields |= motion_recorder::kCommandFieldViewAngles;
    }
    if (base->has_mousedx() || base->has_mousedy())
    {
        command.mouseDx = base->mousedx();
        command.mouseDy = base->mousedy();
        command.fields |= motion_recorder::kCommandFieldMouse;
    }
    if (base->has_weaponselect())
    {
        command.weaponSelect = base->weaponselect() > 0
                                   ? weapon_locker_hooks::WeaponDefForEntityIndex(
                                         pawn_binding::ServicesToWeaponServices(slot, services, nullptr), base->weaponselect())
                                   : 0;
        command.fields |= motion_recorder::kCommandFieldWeaponSelect;
    }
    if (pc->has_left_hand_desired())
    {
        command.leftHandDesired = pc->left_hand_desired() ? 1 : 0;
        command.fields |= motion_recorder::kCommandFieldLeftHand;
    }
    motion_recorder::OnCaptureCommand(slot, command);
}

// Applies one recorded command before the engine simulates it.
void ApplyReplayUserCommand(int slot, void* services, PlayerCommand* pc, CBaseUserCmdPB* base)
{
    constexpr uint64_t kGrenadeAttackMask = (1ULL << 0) | (1ULL << 11);
    motion_recorder::ReplayCommandFrame frame{};
    if (motion_recorder::ReplayCommandFrameForSimulation(slot, frame))
    {
        bool suppressUnsafeUtilityAttack =
            IsThrowableUtilityDef(frame.tick.weaponDefIndex) && frame.weaponSelect < 0 &&
            !motion_recorder::ReplayWeaponDefsMatch(motion_recorder::BotActiveWeaponDef(slot), frame.tick.weaponDefIndex);
        if (suppressUnsafeUtilityAttack)
        {
            frame.buttons0 &= ~kGrenadeAttackMask;
            frame.buttons1 &= ~kGrenadeAttackMask;
            frame.buttons2 &= ~kGrenadeAttackMask;
        }

        CInButtonStatePB* bp = base->mutable_buttons_pb();
        bp->set_buttonstate1(frame.buttons0);
        bp->set_buttonstate2(frame.buttons1);
        bp->set_buttonstate3(frame.buttons2);
        pc->buttonstates.m_pButtonStates[0] = frame.buttons0;
        pc->buttonstates.m_pButtonStates[1] = frame.buttons1;
        pc->buttonstates.m_pButtonStates[2] = frame.buttons2;

        CMsgQAngle* view = base->mutable_viewangles();
        view->set_x(frame.commandView.pitch);
        view->set_y(NormalizeDeg(frame.commandView.yaw));
        view->set_z((frame.commandFields & motion_recorder::kCommandFieldViewAngles) != 0 ? frame.commandView.roll : 0.0F);

        if ((frame.commandFields & motion_recorder::kCommandFieldForwardMove) != 0) base->set_forwardmove(frame.forwardMove);
        if ((frame.commandFields & motion_recorder::kCommandFieldLeftMove) != 0) base->set_leftmove(frame.leftMove);
        if ((frame.commandFields & motion_recorder::kCommandFieldUpMove) != 0) base->set_upmove(frame.upMove);
        if ((frame.commandFields & motion_recorder::kCommandFieldMouse) != 0)
        {
            base->set_mousedx(frame.mouseDx);
            base->set_mousedy(frame.mouseDy);
        }
        if ((frame.commandFields & motion_recorder::kCommandFieldLeftHand) != 0) pc->set_left_hand_desired(frame.leftHandDesired != 0);
        if ((frame.commandFields & motion_recorder::kCommandFieldForwardMove) == 0) base->clear_forwardmove();
        if ((frame.commandFields & motion_recorder::kCommandFieldLeftMove) == 0) base->clear_leftmove();
        if ((frame.commandFields & motion_recorder::kCommandFieldUpMove) == 0) base->clear_upmove();
        if ((frame.commandFields & motion_recorder::kCommandFieldMouse) == 0)
        {
            base->clear_mousedx();
            base->clear_mousedy();
        }
        if ((frame.commandFields & motion_recorder::kCommandFieldLeftHand) == 0) pc->clear_left_hand_desired();
        if (frame.weaponSelect >= 0) base->set_weaponselect(frame.weaponSelect);
        else
            base->clear_weaponselect();

        // Replace the command's subtick moves with the recorded set for this tick
        base->clear_subtick_moves();
        for (int i = 0; i < frame.subtickCount; ++i)
        {
            uint32_t button = frame.subticks[i].button;
            float pressed = frame.subticks[i].pressed;
            if (suppressUnsafeUtilityAttack && (button & static_cast<uint32_t>(kGrenadeAttackMask)) != 0)
            {
                button &= ~static_cast<uint32_t>(kGrenadeAttackMask);
                if (button == 0) pressed = 0.0F;
            }

            CSubtickMoveStep* m = base->add_subtick_moves();
            m->set_when(frame.subticks[i].when);
            m->set_button(button);
            if (button != 0) // digital press/release
                m->set_pressed(pressed != 0.0F);
            if (frame.subticks[i].pitchDelta != 0.0F) m->set_pitch_delta(frame.subticks[i].pitchDelta);
            if (frame.subticks[i].yawDelta != 0.0F) m->set_yaw_delta(frame.subticks[i].yawDelta);
            if (frame.subticks[i].analogForward != 0.0F) m->set_analog_forward_delta(frame.subticks[i].analogForward);
            if (frame.subticks[i].analogLeft != 0.0F) m->set_analog_left_delta(frame.subticks[i].analogLeft);
        }

        // Mark before calling the engine: reentrant commands must not reseed.
        auto* boundary = FindPhysicsFrame(slot);
        if (boundary && !boundary->seeded)
        {
            boundary->seeded = true;
            motion_recorder::OnReplayCommandPre(slot, services, frame.tick);
        }
    }
}

// Records or injects the user command before native simulation.
// Gate 2B diagnostic addition: same pre-hook caveat as HookedProcessMovement
// above -- reaching this function is not proof the original PlayerRunCommand
// subsequently ran. Only reads are added around the existing logic; nothing
// below this comment changes what the existing code does.
KHook::Return<void> HookedPlayerRunCommand(void* services, void* cmd) noexcept
{
    int slot = pawn_binding::ServicesToSlot(services);
    gate2b::RecordPlayerRunCommandEntry(slot, cmd != nullptr); // unconditional; safe even if slot<0 or cmd==nullptr

    auto* boundary = FindPhysicsFrame(slot);
    bool recording = boundary && boundary->recording && motion_recorder::IsRecording(slot);
    bool replaying = boundary && boundary->replaying && motion_recorder::IsReplaying(slot);
    const auto [hasUsercmdInjection, hasUsercmdSuppression, hasUsercmdMovement] = replaying ? UsercmdWork{} : GetUsercmdWork(slot);

    if (cmd == nullptr)
    {
        return { KHook::Action::Ignore };
    }

    // Gate 2B diagnostic "before" snapshot: read-only, deliberately outside
    // and independent of the production gate below, so a call where every
    // production flag is false (the expected state immediately after
    // CancelUsercmdMovement) is still observed. has_base()/base() are the
    // same const, non-mutating accessor pair this file already relies on
    // for every sibling optional field (see has_forwardmove/has_leftmove/
    // has_upmove/has_viewangles/has_mousedx/has_mousedy a few lines below in
    // CaptureUserCommand) -- base() returns a reference to the default
    // instance without allocating when has_base() is false, so this can
    // never mutate the command the way mutable_base() would.
    const auto* pcConst = reinterpret_cast<const PlayerCommand*>(cmd);
    const bool hasBaseBefore = pcConst->has_base();
    const float fwdBefore = hasBaseBefore ? pcConst->base().forwardmove() : 0.0f;
    const float sideBefore = hasBaseBefore ? pcConst->base().leftmove() : 0.0f;
    const int subtickBefore = hasBaseBefore ? pcConst->base().subtick_moves_size() : 0;

    if (recording || replaying || hasUsercmdInjection || hasUsercmdSuppression || hasUsercmdMovement)
    {
        // Exact existing production branch, unchanged.
        // Compiler computes the multiple-inheritance adjust here.
        auto* pc = reinterpret_cast<PlayerCommand*>(cmd);
        CBaseUserCmdPB* base = pc->mutable_base();

        if (recording) CaptureUserCommand(slot, services, pc, base);

        if (replaying) ApplyReplayUserCommand(slot, services, pc, base);

        if (hasUsercmdSuppression && !replaying) ApplyUsercmdSuppressions(slot, pc, base);
        if (hasUsercmdInjection && !replaying) ApplyUsercmdInjections(slot, pc, base);
        if (hasUsercmdMovement && !replaying) ApplyUsercmdMovement(slot, pc, base);
    }

    // Gate 2B diagnostic "after" snapshot: still read-only. If the production
    // branch ran, base is now guaranteed present and this reads the same
    // (already-mutated-by-the-production-code, not by us) submessage through
    // the const accessor; if it didn't run, nothing touched the command, so
    // these equal the "before" values by construction.
    const bool hasBaseAfter = pcConst->has_base();
    const float fwdAfter = hasBaseAfter ? pcConst->base().forwardmove() : 0.0f;
    const float sideAfter = hasBaseAfter ? pcConst->base().leftmove() : 0.0f;

    gate2b::RecordPlayerRunCommandObservation(slot, gate2b::CurrentPhysicsSimulateSeq(slot), MonotonicMilliseconds(),
                                                pcConst->cmdNum, hasUsercmdMovement, hasBaseBefore, fwdBefore,
                                                sideBefore, fwdAfter, sideAfter, subtickBefore);

    return { KHook::Action::Ignore };
}

// ---- PhysicsSimulate: the per-tick boundary ----
// Records pre/post + commits

// Dispatches drops after native command setup, at the client-command boundary.
KHook::Return<void> ControllerCommandSetupPost(void* controller) noexcept
{
    if (g_physicsFrames.empty()) return { KHook::Action::Ignore };
    // Do not cross an inactive/nested simulation frame to consume an outer event.
    const MovementFrame frame = g_physicsFrames.back();
    if (frame.replaying && frame.services && ControllerToSlot(controller) == frame.slot && motion_recorder::IsReplaying(frame.slot))
        ApplyReplayDrop(frame.slot, frame.services);
    return { KHook::Action::Ignore };
}

// Resolves the controller's per-command setup independently of movement services.
void EnsureControllerCommandHook(void* controller)
{
    if (!controller || g_controllerHookTried.exchange(true, std::memory_order_acq_rel)) return;
    void** vtable = nullptr;
    void* target = nullptr;
    if (tg::g_vtIdxControllerCommandSetup >= 0 && GuardedRead(controller, 0, vtable) && vtable &&
        GuardedRead(static_cast<const void*>(vtable), tg::g_vtIdxControllerCommandSetup * static_cast<int>(sizeof(void*)), target) &&
        target && g_hookControllerCommandSetup.Install(target, nullptr, &ControllerCommandSetupPost))
        return;
    g_hookControllerCommandSetup.Remove();
    BC_LOG_WARN("Controller command setup hook unavailable; recording and replay are disabled\n");
}

// Captures the per-tick state before any subtick movement.
KHook::Return<void> HookedPhysicsSimulate(void* controller) noexcept
{
    EnsureControllerCommandHook(controller);

    // Gate 2B diagnostic only: an always-incrementing per-slot call counter,
    // independent of the recording/replay gate immediately below (which
    // stays completely unmodified). The existing frame push/pop mechanism
    // below only creates a real, slot-stamped frame when recording or replay
    // is active -- never true during Gate 2B -- so it cannot serve as a
    // correlation identifier for this diagnostic; this counter replaces that
    // role. It is a LOCAL CALL-SEQUENCE NUMBER (the Nth time this hook fired
    // for this slot since the current diagnostic window began), not the
    // engine's own tick counter, and does not assume one call equals one
    // engine tick. ControllerToSlot is the same read-only helper already
    // called a few lines below for the existing, unmodified logic.
    gate2b::RecordPhysicsSimulateCall(ControllerToSlot(controller));

    if (!motion_recorder::HasAnyRecording() && !motion_recorder::HasAnyReplay())
    {
        // Keep the matching post callback from consuming an outer frame.
        g_physicsFrames.push_back({ -1, nullptr, false, false });
        return { KHook::Action::Ignore };
    }
    int slot = ControllerToSlot(controller);

    void* services = (slot >= 0 && slot < kMaxSlots) ? g_slotServices[slot].load(std::memory_order_acquire) : nullptr;

    bool recording = slot >= 0 && slot < kMaxSlots && services && motion_recorder::IsRecording(slot);
    bool replaying = slot >= 0 && slot < kMaxSlots && services && motion_recorder::IsReplaying(slot);

    // A nested simulation for this slot belongs to the existing outer boundary.
    if (FindPhysicsFrame(slot))
    {
        g_physicsFrames.push_back({ -1, nullptr, false, false });
        return { KHook::Action::Ignore };
    }
    // Publish ownership before engine callbacks can reenter.
    g_physicsFrames.push_back({ slot, services, recording, replaying });

    // pre: snapshot start-of-tick state once (before any subtick mover).
    if (recording) motion_recorder::OnCapturePre(slot, services, nullptr);

    return { KHook::Action::Ignore };
}

// Commits the recording and replay state for the matching simulation call.
KHook::Return<void> PhysicsSimulatePost(void*) noexcept
{
    const auto [slot, services, recording, replaying, seeded] = g_physicsFrames.back();
    g_physicsFrames.pop_back();

    // post: snapshot end-of-tick state + commit one frame
    if (recording) motion_recorder::OnCapturePost(slot, services, nullptr);
    if (replaying)
    {
        motion_recorder::OnReplayCommit(slot, services, seeded);
    }
    return { KHook::Action::Ignore };
}

std::atomic<bool> g_vtHooksTried{ false };

void EnsureVtableHooks(void* services)
{
    if (g_vtHooksTried.load(std::memory_order_acquire)) return;
    if (g_vtHooksTried.exchange(true, std::memory_order_acq_rel)) return;
    if (!services) return;
    void** vt = nullptr;
    if (!GuardedRead(services, 0, vt) || !vt) return;

    // PlayerRunCommand (subtick record/re-inject)
    if (!GuardedRead(static_cast<const void*>(vt), tg::g_vtIdxPlayerRunCommand * static_cast<int>(sizeof(void*)), g_addrPlayerRunCommand))
        g_addrPlayerRunCommand = nullptr;
    if (g_addrPlayerRunCommand && g_hookPlayerRunCommand.Install(g_addrPlayerRunCommand, &HookedPlayerRunCommand))
    {
        g_subtickActive = true;
    }
    else if (g_addrPlayerRunCommand)
    {
        g_hookPlayerRunCommand.Remove();
        g_addrPlayerRunCommand = nullptr;
    }
}

} // namespace

bool Install( // NOLINT(misc-use-internal-linkage)
    const nlohmann::json& gd,
    const modules::ModuleInfo& serverModule,
    char* errorOut,
    size_t errorOutLen) // NOLINT(misc-use-internal-linkage)
{
    g_addrProcessMovement = gameconfig::ResolveSig(gd, serverModule, "CCSPlayer_MovementServices::ProcessMovement", errorOut, errorOutLen);
    if (!g_addrProcessMovement)
    {
        g_status = "failed: ProcessMovement sig";
        return false;
    }
    if (!g_hookProcessMovement.Install(g_addrProcessMovement, &HookedProcessMovement, &HookedProcessMovementPost))
    {
        std::snprintf(errorOut, errorOutLen, "hook ProcessMovement failed");
        g_hookProcessMovement.Remove();
        g_status = "failed: hook ProcessMovement";
        return false;
    }

    // PhysicsSimulate: the per-tick boundary
    char psErr[256] = { 0 };
    g_addrPhysicsSimulate = gameconfig::ResolveSig(gd, serverModule, "CBasePlayerController::OnSimulateUserCommands", psErr, sizeof(psErr));
    if (g_addrPhysicsSimulate && g_hookPhysicsSimulate.Install(g_addrPhysicsSimulate, &HookedPhysicsSimulate, &PhysicsSimulatePost))
    {
        g_physicsActive = true;
    }
    else
    {
        if (g_addrPhysicsSimulate)
        {
            g_hookPhysicsSimulate.Remove();
            g_addrPhysicsSimulate = nullptr;
        }
        BC_LOG_WARN("PhysicsSimulate hook unavailable (%s); recording and replay are disabled\n", psErr[0] ? psErr : "KHook failed");
    }

    // PlayerRunCommand is hooked lazily from the first live movement-services vtable.
    g_installed = true;
    g_status = "ok";
    return true;
}

void Remove()
{
    if (!g_installed) return;
    g_hookProcessMovement.Remove();
    g_hookPlayerRunCommand.Remove();
    g_hookControllerCommandSetup.Remove();
    g_hookPhysicsSimulate.Remove();
    g_addrProcessMovement = nullptr;
    g_addrPlayerRunCommand = nullptr;
    g_addrPhysicsSimulate = nullptr;
    g_physicsActive = false;
    g_subtickActive = false;
    g_vtHooksTried.store(false, std::memory_order_release);
    g_controllerHookTried.store(false, std::memory_order_release);
    for (auto& s : g_slotServices)
        s.store(nullptr, std::memory_order_release);
    pawn_binding::ClearAll();
    {
        std::scoped_lock lock(g_usercmdInjectionMutex);
        for (auto& injections : g_usercmdInjections)
            injections.clear();
        for (auto& suppressions : g_usercmdSuppressions)
            suppressions.clear();
        for (auto& movements : g_usercmdMovements)
            movements.clear();
        g_injectedHeldMasks.fill(0);
        g_movementHeldMasks.fill(0);
    }
    g_installed = false;
    g_status = "not_attempted";
}

// Recording and replay require frame, client-command, and movement boundaries.
bool RecorderReady()
{
    if (g_physicsActive && g_subtickActive && g_hookControllerCommandSetup.Active()) return true;
    BC_LOG_WARN("Cannot start recording/replay: PhysicsSimulate=%d PlayerRunCommand=%d ControllerCommandSetup=%d\n", g_physicsActive,
                g_subtickActive, g_hookControllerCommandSetup.Active());
    return false;
}

const char* Status() { return g_status.c_str(); }

} // namespace input_injector
} // namespace cs2bc

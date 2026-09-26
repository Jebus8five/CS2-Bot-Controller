// Gate 2B diagnostic-only instrumentation. Not part of the production HOS
// control surface: nothing here changes gameplay, movement, or hook
// semantics for PlayerRunCommand/ProcessMovement/PhysicsSimulate. It only
// records bounded, read-only observations of calls that already happen.
//
// Lifecycle is owned entirely by the caller (the PoC harness, via three new
// exports in exports.cpp): DiagnosticStart -> ... -> DiagnosticMarkCancelled
// -> ... -> DiagnosticFinalize. No function in this file blocks on an OS
// synchronization primitive or drives a deadline; DiagnosticStart/Finalize
// use a small BOUNDED spin (a fixed iteration cap, never unbounded) to
// safely drain in-flight writers -- see the writer-drain protocol comment
// above DiagnosticFinalize in the .cpp for the race analysis.
#pragma once

#include <cstdint>

namespace cs2bc::gate2b {

// Status codes returned by DiagnosticStart/DiagnosticFinalize.
constexpr int kStatusOk = 0;
constexpr int kStatusRepeat = 1;      // finalize only: harmless repeat, re-emitted the frozen result
constexpr int kStatusInvalidSlot = 2;
constexpr int kStatusBusy = 3;        // bounded drain could not confirm quiescence in time;
                                       // buffers were NOT read/reset -- caller should retry

// Resets this slot's counters/buffers and begins capture (phase=Activation).
// Safe to call repeatedly. Performs a bounded drain of any writer left over
// from a previous, un-finalized window before resetting; if that drain does
// not complete in time, returns kStatusBusy and leaves the previous window's
// state untouched (does NOT reset while a writer could still be active).
int DiagnosticStart(int slot);

// Explicit, harness-driven transition: marks that HOS movement injection has
// been cancelled for this slot. Does NOT stop capture and does NOT dump
// anything. Only meaningful while a capture is active; otherwise a no-op.
// This is deliberately a separate call from CancelUsercmdMovement so that an
// injection failure observed during Activation/SteadyState (hasUsercmdMovement
// becoming false for reasons other than an intentional cancel) can never be
// mistaken for entering the Cancellation/PostCancel phase.
void DiagnosticMarkCancelled(int slot);

// Closes admission to new writers, performs a bounded drain of any writer
// already admitted, and only then emits one bounded summary via the existing
// BC_LOG_* sink. Idempotent: a second call for the same slot re-emits the
// same already-frozen snapshot rather than recomputing or mutating anything.
// aborted!=0 labels the emitted summary PARTIAL/ABORTED instead of COMPLETE.
// If the bounded drain cannot confirm quiescence, returns kStatusBusy WITHOUT
// reading or resetting the buffers, and does NOT mark the slot finalized --
// admission stays closed either way, so no new samples are lost, and the
// caller may safely retry the call.
int DiagnosticFinalize(int slot, int aborted);

// ---- Recording calls, made from the existing hooks ----

// Unconditional: called at the very top of HookedPlayerRunCommand/
// HookedProcessMovement, before any other logic, regardless of whether a
// diagnostic capture is active. These are the counters that must be
// observable as a genuine zero if a hook never fires at all.
void RecordPlayerRunCommandEntry(int slot, bool hadCmd);
void RecordProcessMovementEntry(int slot);

// Always-on, diagnostic-only per-slot call counter for HookedPhysicsSimulate,
// read independently of the existing recording/replay gate inside that hook
// (which stays completely unmodified).
//
// IMPORTANT, precise scope of what this represents: CurrentPhysicsSimulateSeq
// returns whatever RecordPhysicsSimulateCall's counter holds AT THE MOMENT it
// is read from RecordPlayerRunCommandObservation/RecordProcessMovementObservation
// -- i.e. "how many times HookedPhysicsSimulate has fired for this slot, as of
// an arbitrary later read". It is NOT a verified active physics frame, and it
// does NOT establish that the PRC/PM call reading it happened "within" any
// particular PhysicsSimulate invocation's boundary -- there is no push/pop or
// scoping relationship here at all, only two independently-incrementing
// counters read at different times. Treat this value as a coarse, same-slot
// sequencing hint only (e.g. "roughly how far into the test this sample was
// taken, in PhysicsSimulate-call terms"), never as frame-level or
// command-level correlation. The load-bearing correlation for Gate 2B is,
// and remains, slot + the phase/windowSeq fields on each sample + the
// bounded observation window each phase represents -- not this counter.
void RecordPhysicsSimulateCall(int slot);
uint32_t CurrentPhysicsSimulateSeq(int slot);

// Detailed observation, called whenever cmd != nullptr in
// HookedPlayerRunCommand, independent of the production injection gate, so
// post-cancellation calls (where every production feature flag is false)
// are still observed. hasBase distinguishes "no base submessage present"
// from "base present with zero/default values" -- callers must pass 0.0f/0
// for the before/after/subtick fields when hasBase is false, and this
// function preserves that distinction in the recorded sample rather than
// treating an absent base as an observed zero.
//
// heldMaskBefore/heldMaskAfter are the raw pc->buttonstates.m_pButtonStates[0]
// value (the same held-button mask ApplyUsercmdMovement already reads/writes
// in InputInjector.cpp), read before and after the production branch,
// independent of hasBase. This is a RAW, uninterpreted 64-bit mask: no bit
// position here is asserted to mean anything (in particular, no bit is
// asserted to be a "walk"/IN_SPEED indicator) -- only kInForward=1<<3,
// kInBack=1<<4, kInMoveLeft=1<<9, kInMoveRight=1<<10, and the grenade-related
// (1<<0)|(1<<11) mask are independently verified in this codebase today (see
// InputInjector.cpp). Any other bit read from this field is a hypothesis to
// be tested against logged data, not a fact encoded here.
void RecordPlayerRunCommandObservation(int slot, uint32_t physicsSimulateSeq, int64_t nowMs, int cmdNum,
                                        bool hasUsercmdMovementObserved, bool hasBase, float fwdBefore,
                                        float sideBefore, float fwdAfter, float sideAfter, int subtickCountBefore,
                                        uint64_t heldMaskBefore, uint64_t heldMaskAfter);

// Detailed observation from HookedProcessMovement's pre-hook, once the
// second argument (CMoveData*, independently verified this session via its
// own +0x2C/+0x30/+0xC8 data flow, not merely its argument position) is
// known non-null.
void RecordProcessMovementObservation(int slot, uint32_t physicsSimulateSeq, int64_t nowMs, float forwardMove,
                                       float sideMove, float velX, float velY, float velZ);

} // namespace cs2bc::gate2b

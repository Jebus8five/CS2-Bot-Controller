// Gate 2C diagnostic-only instrumentation. Additive to, and independent
// of, Gate2BDiagnostics: no line of Gate2BDiagnostics.h/.cpp is modified,
// and Gate2B's own recording call sites in InputInjector.cpp are
// unchanged. This file reuses the exact bounded-buffer / atomic-claim /
// writer-drain PATTERN already reviewed and race-fixed for Gate2B (see
// Gate2BDiagnostics.cpp's own race analysis A-F), applied to a small,
// separate Gate2C sample type and its own independent per-slot state --
// it does not read or write any Gate2B state.
//
// Objective: determine whether writing the movement intent directly into
// CMoveData inside the ProcessMovement pre-hook (immediately before the
// original ProcessMovement body runs) produces sustained physical
// movement, as opposed to the PlayerRunCommand-only injection Gate2B
// measured.
//
// PRE/POST pairing model (corrected from an earlier single-staged-slot
// design after review): KHook's own header (Kenzzer/KHook, upstream of
// this project's vendored copy) documents no guarantee that a pre and
// post callback for one call cannot interleave with another call to the
// same hooked function, and contains an explicit developer comment
// acknowledging reentrant "recalls" as a real hazard ("This can deadlock
// (in case of recalls) / so make a deep-copy"). This project's own
// InputInjector.cpp already defends against exactly this class of hazard
// for PhysicsSimulate via a thread_local stack (g_physicsFrames). Gate2C
// follows that same established pattern for its own per-invocation
// frames (see the thread_local Gate2CFrame stack in InputInjector.cpp),
// but -- per explicit review instruction -- does NOT assume thread_local
// alone makes pairing safe: each frame carries slot, services and
// moveData identity, a globally unique invocation id, and the per-slot
// diagnostic generation it was created under, and POST verifies identity
// (not just stack position) before treating a frame as its match.
// Unmatched, orphaned (nested-but-abandoned), overflowed, and
// stale-generation frames are all counted, never silently recorded as if
// they were a normal paired observation.
//
// Lifecycle, owned by the caller (three new exports mirroring Gate2B's):
//   Start(slot, writeScale) -> ... -> MarkCancelled(slot) -> ...
//   -> Finalize(slot, aborted)
//
// Disabled by default: IsActiveForSlot(slot) is false until Start has
// been called for that slot, and false again after Finalize -- this is
// the switch InputInjector.cpp's PRE hook checks before pushing any new
// frame or writing CMoveData. The POST hook does NOT gate on this flag
// (see CurrentGeneration below) -- it must still be able to pop and
// safely discard a frame that was pushed just before Finalize ran, so it
// is never leaked in the thread_local stack.
#pragma once

#include <cstdint>

namespace cs2bc::gate2c {

constexpr int kStatusOk = 0;
constexpr int kStatusRepeat = 1;
constexpr int kStatusInvalidSlot = 2;
constexpr int kStatusBusy = 3;

// Begins a Gate2C window for slot, advancing that slot's diagnostic
// generation counter. Intended to be called only after the caller has
// already put the slot under Lock(All) and started a Gate2B window --
// Gate2C shares no state with Gate2B, but is meant to run alongside it
// (both hooks observe the same real ProcessMovement calls), not instead
// of it.
//
// writeScale is the explicit, test-supplied multiplier applied to the
// shared usercmd movement intent (see InputInjector.cpp's existing
// g_usercmdMovements) before it is written to CMoveData. It is NOT
// assumed to be 450 (the legacy usercmd scale), nor is 1.0 asserted as
// the established CMoveData unit -- both remain untested hypotheses; the
// caller supplies the value explicitly and it is recorded in every
// sample so results are always interpretable against the value actually
// used.
//
// Returns kStatusBusy (and leaves any previous window's buffers and
// generation untouched) if a bounded drain of a still-active prior
// writer does not complete in time; the caller may safely retry.
int Start(int slot, float writeScale);

// Marks that the shared usercmd movement intent has been (or is about
// to be) cancelled for this slot. Same semantics as Gate2B's
// MarkCancelled: does not stop capture, only shifts phase bucketing from
// Activation/SteadyState to Cancellation/PostCancel.
void MarkCancelled(int slot);

// Closes admission, performs a bounded drain of any in-flight writer,
// and emits the bounded summary via BC_LOG_INFO. Idempotent. Returns
// kStatusBusy (without touching buffers) if the drain does not complete
// in time. Does NOT reach into any thread's thread_local frame stack --
// see CurrentGeneration for how a frame pushed just before this call is
// still handled safely if its POST arrives afterward.
int Finalize(int slot, int aborted);

// True only between a slot's own successful Start and its Finalize. Read
// by the PRE hook, fresh, on every ProcessMovement invocation, to decide
// whether to push a new frame and write CMoveData at all. Toggling it
// never requires a rebuild or restart.
bool IsActiveForSlot(int slot);

// The explicit write-scale currently in effect for slot (0 if the slot is
// not active). This is the SAME value the caller passed to Start -- there
// is no second, silently-different scale computed anywhere else.
float WriteScaleForSlot(int slot);

// This slot's current diagnostic generation, advanced once per successful
// Start. The PRE hook stamps this onto every frame it pushes; RecordResult
// (below) compares a completed frame's stamped generation against the
// CURRENT value of this counter at commit time and discards the record
// (counted, not silently dropped) if they differ -- this is what
// guarantees a POST belonging to an earlier test can never land in a
// later one's buffers, without requiring any cross-thread access to the
// PRE hook's thread_local stack.
uint32_t CurrentGeneration(int slot);

// Allocates a new, globally unique (process-lifetime) invocation id for
// one PRE hook call. Safe to call from any thread.
uint64_t NextInvocationId();

// ---- Recording -- called only from InputInjector.cpp's ProcessMovement
// hooks. ----

// Called from the PRE hook when the bounded thread_local stack was
// already at capacity for a new invocation on that thread. Eviction was
// considered and rejected: evicting the oldest entry does not prevent an
// evicted invocation's eventual (possibly much later) POST from matching
// a DIFFERENT, unrelated frame that later reuses the same identity --
// only "no match" is safe to conclude from eviction, not "the match I
// found is correct." Instead, this marks the affected thread POISONED
// for the (slot, generation) pair in effect at the moment of overflow
// (see g_gate2cPoisoned in InputInjector.cpp): every pending frame on
// that thread is wiped, no further CMoveData write or frame push happens
// for that (slot, generation) on that thread, and the overall diagnostic
// result for slot is marked INVALID (not merely "some samples dropped")
// the next time Finalize runs -- see EmitSummary. A fresh Start() (a new
// generation) clears the poison automatically.
void RecordOverflowPoisoned(int slot);

// Called from the POST hook when no frame matching this call's identity
// (slot, services, moveData) could be found anywhere in the bounded
// thread_local stack -- e.g. Gate2C became active/inactive between this
// call's PRE and POST, or this thread was poisoned (see
// RecordOverflowPoisoned) after this call's PRE ran, wiping its frame.
// No sample is recorded for this invocation.
void RecordUnmatchedPost(int slot);

// Called from the POST hook once per frame that had to be discarded
// because it sat ABOVE the frame that actually matched this POST's
// identity in the stack -- i.e. a nested/reentrant invocation whose own
// POST never arrived (or arrived out of order) before this one did.
// These are never merged into any recorded sample.
void RecordOrphanedFrame(int slot);

// Called from the POST hook when a matching frame WAS found (a correct,
// non-mismatched pairing) but that frame carries no data -- it was pushed
// as a non-recording marker because Gate2C was inactive when this
// invocation's PRE ran (see InputInjector.cpp's Gate2CFrame comment for
// why a marker is pushed at all in that case). This is an expected,
// benign outcome, not a pairing failure, and is counted separately from
// overflow/unmatchedPost/orphanedFrames/staleGeneration so it is never
// misread as one.
void RecordNonRecordingMatch(int slot);

// Called from the POST hook once a matching frame has been found AND its
// generation still equals CurrentGeneration(slot) at this exact moment.
// Performs the one bounded, atomically-claimed write for this invocation.
// If the generation no longer matches (a Start/Finalize happened between
// this frame's PRE and this call), the record is discarded and counted
// as a stale-generation drop instead of being written -- this is checked
// again, redundantly but cheaply, inside this call itself (not only by
// the caller), so there is exactly one place this guarantee is enforced.
void RecordResult(int slot, uint32_t frameGeneration, uint64_t invocationId, int64_t nowMs, float preForward,
                   float preSide, bool wroteIntent, float injectedForward, float injectedSide, bool lockAllActive,
                   float postForward, float postSide, float velX, float velY, float velZ, float originX,
                   float originY, float originZ);

} // namespace cs2bc::gate2c

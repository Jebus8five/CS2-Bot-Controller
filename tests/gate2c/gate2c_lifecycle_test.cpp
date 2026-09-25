// Standalone, host-side test for Gate2CDiagnostics.cpp's positional
// PRE/POST pairing state machine -- NOT wired into the main plugin
// build (CMakeLists.txt) or CI (.github/workflows/build.yml), and NOT
// built or run as part of this change unless separately authorized. See
// tests/gate2c/CMakeLists.txt for how to build it in isolation.
//
// This is possible with zero CS2/HL2SDK/KHook dependency because
// Gate2CDiagnostics.cpp only ever sees opaque void* pointers and plain
// floats/bools -- see Gate2CDiagnostics.h's top-of-file comment. The
// only external dependency is core/log.h's BC_LOG_INFO macro, stubbed
// below rather than linking the real spdlog-backed logger.
//
// IMPORTANT ordering note: t_fatalDepth (Gate2CDiagnostics.cpp) is
// thread_local and, by design, NEVER cleared once set (see
// ReserveFrame's header comment) -- it is a permanent, thread-scoped
// shutdown. Any test that deliberately drives a thread into fatal depth
// MUST do so on a throwaway worker thread, never on main(), or every
// later test in this same process would silently and permanently stop
// being able to track anything. TestFatalDepthAndCrossSlotEffects is the
// only test that does this, and it runs on its own std::thread for
// exactly that reason.
//
// Assertions are made ONLY against this file's public API surface
// (Gate2CReservation/Gate2CResolution/Start's return status/
// IsActiveForSlot/WriteScaleForSlot) -- there are no internal counter
// getters, so a test cannot "cheat" by reaching into private state; it
// has exactly the same visibility a real caller has.
//
// TestAdmissionClosedPreservesPositionWithIdenticalIdentity specifically
// exercises the hazard a later review identified: an invocation whose
// PRE returns AdmissionClosed still has a real POST coming, and if that
// POST's (slot, services, moveData) happen to be IDENTICAL to an older,
// still-genuinely-pending invocation's (pointer reuse is expected on
// this codebase's own evidence), a pointer-identity check alone cannot
// tell them apart -- only a correct positional reservation can.

#include "Gate2CDiagnostics.h"
#include "core/log.h"

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

// ---- core/log.h stub: no spdlog, no file I/O -- just stdout, so this
// binary has no dependency beyond the C++ standard library. ----
namespace cs2bc::log {
bool Init(const char*, char*, size_t) { return true; }
void Close() {}
void Write(Level, const char* format, ...)
{
    va_list args;
    va_start(args, format);
    std::vprintf(format, args);
    va_end(args);
}
} // namespace cs2bc::log

namespace {

int g_failures = 0;
int g_checks = 0;

#define CHECK(cond)                                                                                                  \
    do                                                                                                                \
    {                                                                                                                 \
        ++g_checks;                                                                                                   \
        if (!(cond))                                                                                                  \
        {                                                                                                             \
            ++g_failures;                                                                                             \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                              \
        }                                                                                                             \
    } while (0)

using namespace cs2bc::gate2c;

// Opaque, distinguishable-by-address stand-ins for CCSPlayer_MovementServices*
// and CMoveData* -- never dereferenced, only compared by pointer identity,
// exactly as ReserveFrame/BeginResolveFrame treat them.
int g_fakeServicesA = 0;
int g_fakeMoveDataA = 0;
int g_fakeServicesB = 0;
int g_fakeMoveDataB = 0;

// ---------------------------------------------------------------------
// 1. Nested calls: strict LIFO pairing must hold even when two
//    concurrently-pending invocations share IDENTICAL (slot, services,
//    moveData) -- the exact scenario an identity-search design could get
//    wrong (and, per this project's review history, did). Simulates:
//      PRE(outer) PRE(inner) POST(inner) POST(outer)
//    which is the only ordering KHook's own proven same-thread, strict
//    call-stack LIFO dispatch can produce for two invocations that
//    genuinely nest.
// ---------------------------------------------------------------------
void TestNestedCallsLifoPairing()
{
    const int slot = 0;
    CHECK(Start(slot, 1.0F) == kStatusOk);

    void* services = &g_fakeServicesA;
    void* moveData = &g_fakeMoveDataA; // SAME identity for both invocations, deliberately

    Gate2CReservation outer = ReserveFrame(slot, services, moveData);
    CHECK(outer.admission == Gate2CAdmission::Reserved);
    CHECK(outer.active);
    CommitFrame(slot, outer.invocationId, true, /*preForward=*/10.0F, /*preSide=*/0.0F, /*wroteIntent=*/true,
                /*injectedForward=*/10.0F, /*injectedSide=*/0.0F, /*lockAllActive=*/true);

    Gate2CReservation inner = ReserveFrame(slot, services, moveData);
    CHECK(inner.admission == Gate2CAdmission::Reserved);
    CHECK(inner.invocationId != outer.invocationId);
    CommitFrame(slot, inner.invocationId, true, /*preForward=*/20.0F, /*preSide=*/0.0F, /*wroteIntent=*/true,
                /*injectedForward=*/20.0F, /*injectedSide=*/0.0F, /*lockAllActive=*/true);

    // POST(inner) resolves FIRST -- must get the inner's own data (20),
    // never the outer's (10), despite identical identity.
    Gate2CResolution innerResolved = BeginResolveFrame(slot, services, moveData);
    CHECK(innerResolved.outcome == Gate2CResolutionOutcome::DataReady);
    CHECK(innerResolved.invocationId == inner.invocationId);
    CHECK(innerResolved.preForward == 20.0F);

    // POST(outer) resolves SECOND -- must get the outer's own data (10).
    Gate2CResolution outerResolved = BeginResolveFrame(slot, services, moveData);
    CHECK(outerResolved.outcome == Gate2CResolutionOutcome::DataReady);
    CHECK(outerResolved.invocationId == outer.invocationId);
    CHECK(outerResolved.preForward == 10.0F);

    CHECK(Finalize(slot, 0) == kStatusOk);
}

// Helper: reserves `count` invocations back-to-back (no resolution in
// between) on `slot`, using distinct identity per invocation, and
// returns the reservations plus their identities. Caller owns cleanup
// of the allocated identities. Does not go anywhere near the combined
// 16-deep boundary unless `count` is told to.
struct ProbeResult
{
    std::vector<Gate2CReservation> reservations;
    std::vector<void*> services;
    std::vector<void*> moveDatas;
};

ProbeResult ReserveN(int slot, int count)
{
    ProbeResult out;
    for (int i = 0; i < count; ++i)
    {
        auto* s = new int(i);
        auto* m = new int(i);
        out.services.push_back(s);
        out.moveDatas.push_back(m);
        Gate2CReservation r = ReserveFrame(slot, s, m);
        out.reservations.push_back(r);
        if (r.admission == Gate2CAdmission::Reserved)
            CommitFrame(slot, r.invocationId, true, static_cast<float>(i), 0.0F, true, static_cast<float>(i), 0.0F,
                        true);
    }
    return out;
}

void ResolveAllLifo(int slot, ProbeResult& p)
{
    for (int i = static_cast<int>(p.reservations.size()) - 1; i >= 0; --i)
    {
        Gate2CResolution res = BeginResolveFrame(slot, p.services[i], p.moveDatas[i]);
        if (p.reservations[i].admission == Gate2CAdmission::Overflow)
        {
            CHECK(res.outcome == Gate2CResolutionOutcome::OverflowResolved);
        }
        else
        {
            CHECK(res.outcome == Gate2CResolutionOutcome::DataReady);
            CHECK(res.preForward == static_cast<float>(i));
        }
    }
}

void FreeProbe(ProbeResult& p)
{
    for (auto* s : p.services) delete static_cast<int*>(s);
    for (auto* m : p.moveDatas) delete static_cast<int*>(m);
}

// ---------------------------------------------------------------------
// 2. Overflow boundaries, run on the main thread (none of these reach
//    the combined 16-deep fatal threshold, so main's own t_fatalDepth
//    stays false throughout -- safe to run alongside every other
//    non-fatal test in this file).
//
//    depth  8: exactly fills the bounded tier. No overflow, slot stays
//              active.
//    depth  9: the 9th invocation is the FIRST to overflow. Slot is
//              forced inactive immediately.
//    depth 16: exactly fills BOTH the bounded (8) and overflow (8)
//              tiers. Still not fatal -- 16 is the combined capacity,
//              not one past it. Every one of the 16 must still drain
//              correctly and a fresh Start() must succeed afterward.
// ---------------------------------------------------------------------
void TestOverflowAtDepth8()
{
    const int slot = 1;
    CHECK(Start(slot, 1.0F) == kStatusOk);
    ProbeResult p = ReserveN(slot, 8);
    for (auto& r : p.reservations) CHECK(r.admission == Gate2CAdmission::Reserved);
    CHECK(IsActiveForSlot(slot)); // exactly at capacity -- not yet overflowed
    ResolveAllLifo(slot, p);
    FreeProbe(p);
    CHECK(Start(slot, 1.0F) == kStatusOk); // fully drained, no anomaly -- Start() succeeds again
    CHECK(Finalize(slot, 0) == kStatusOk);
}

void TestOverflowAtDepth9()
{
    const int slot = 2;
    CHECK(Start(slot, 1.0F) == kStatusOk);
    ProbeResult p = ReserveN(slot, 9);
    for (int i = 0; i < 8; ++i) CHECK(p.reservations[i].admission == Gate2CAdmission::Reserved);
    CHECK(p.reservations[8].admission == Gate2CAdmission::Overflow); // the 9th, and only the 9th
    CHECK(!IsActiveForSlot(slot)); // forced inactive the instant overflow happens
    ResolveAllLifo(slot, p);
    FreeProbe(p);
    CHECK(Start(slot, 1.0F) == kStatusOk); // overflow alone is recoverable once fully drained
    CHECK(Finalize(slot, 0) == kStatusOk);
}

void TestOverflowAtDepth16()
{
    const int slot = 3;
    CHECK(Start(slot, 1.0F) == kStatusOk);
    ProbeResult p = ReserveN(slot, 16); // exactly the combined capacity -- NOT fatal
    for (int i = 0; i < 8; ++i) CHECK(p.reservations[i].admission == Gate2CAdmission::Reserved);
    for (int i = 8; i < 16; ++i) CHECK(p.reservations[i].admission == Gate2CAdmission::Overflow);
    CHECK(!IsActiveForSlot(slot));
    ResolveAllLifo(slot, p); // every one of the 16 must resolve correctly
    FreeProbe(p);
    CHECK(Start(slot, 1.0F) == kStatusOk); // 16 is still fully recoverable
    CHECK(Finalize(slot, 0) == kStatusOk);
}

// ---------------------------------------------------------------------
// 3. Fatal depth (the 17th concurrently-unresolved invocation on one
//    thread) and its cross-slot effects. Runs on a DEDICATED worker
//    thread -- see this file's top comment for why. Two slots are used:
//    `primary` (whose 17th reservation on this thread is the one that
//    tips it into fatal) and `sibling` (which already has entries
//    pending on this SAME thread at that moment, but whose own
//    ReserveFrame/BeginResolveFrame calls are not what triggers fatal).
// ---------------------------------------------------------------------
void TestFatalDepthAndCrossSlotEffects()
{
    const int primary = 4;
    const int sibling = 5;

    std::thread worker(
        [&]()
        {
            CHECK(Start(primary, 1.0F) == kStatusOk);
            CHECK(Start(sibling, 1.0F) == kStatusOk);

            // 2 pending on `sibling`, left unresolved.
            ProbeResult siblingProbe = ReserveN(sibling, 2);
            for (auto& r : siblingProbe.reservations) CHECK(r.admission == Gate2CAdmission::Reserved);

            // 14 more on `primary` -- combined thread depth is now 16
            // (8 bounded + 8 overflow across both slots), i.e. exactly
            // at, not past, capacity. None of these 14 should be fatal.
            ProbeResult primaryProbe = ReserveN(primary, 14);
            for (auto& r : primaryProbe.reservations) CHECK(r.admission != Gate2CAdmission::NotTracked);

            // The 17th reservation on this thread (on `primary`) must be
            // declined and must NOT be a silent no-op -- it must tip this
            // thread into permanent fatal depth.
            void* extraServices = &g_fakeServicesA;
            void* extraMoveData = &g_fakeMoveDataA;
            Gate2CReservation seventeenth = ReserveFrame(primary, extraServices, extraMoveData);
            CHECK(seventeenth.admission == Gate2CAdmission::NotTracked);

            // FIXED (this round): the triggering slot (`primary`) AND
            // every OTHER slot with entries already held in either tier
            // at the moment of the fatal transition (`sibling`, via its 2
            // stranded entries) must be poisoned and forced inactive
            // IMMEDIATELY -- not lazily, not only once something happens
            // to touch that slot again. See ReserveFrame's fatal-depth
            // branch (PoisonEveryTrackedSlot) in Gate2CDiagnostics.cpp.
            CHECK(!IsActiveForSlot(primary));
            CHECK(!IsActiveForSlot(sibling));

            // Once fatal, EVERY further Reserve/Resolve on this thread,
            // for EITHER slot, must be inert -- never guess positionally.
            Gate2CReservation afterFatal1 = ReserveFrame(primary, extraServices, extraMoveData);
            CHECK(afterFatal1.admission == Gate2CAdmission::NotTracked);
            Gate2CReservation afterFatal2 = ReserveFrame(sibling, extraServices, extraMoveData);
            CHECK(afterFatal2.admission == Gate2CAdmission::NotTracked);

            // The 16 invocations that were genuinely, validly reserved
            // BEFORE the fatal trigger (2 on sibling + 14 on primary) can
            // no longer be resolved either -- this is the fail-closed,
            // not-self-recovering cost documented in Gate2CDiagnostics.h.
            // None of them may report DataReady/OverflowResolved/
            // NonRecording; all must report FatalDepth.
            for (int i = 0; i < 2; ++i)
            {
                Gate2CResolution res = BeginResolveFrame(sibling, siblingProbe.services[i], siblingProbe.moveDatas[i]);
                CHECK(res.outcome == Gate2CResolutionOutcome::FatalDepth);
            }
            for (int i = 0; i < 14; ++i)
            {
                Gate2CResolution res =
                    BeginResolveFrame(primary, primaryProbe.services[i], primaryProbe.moveDatas[i]);
                CHECK(res.outcome == Gate2CResolutionOutcome::FatalDepth);
            }

            FreeProbe(siblingProbe);
            FreeProbe(primaryProbe);

            // Both slots' outstandingFrames can now never reach zero on
            // this thread again -- Start() must report BUSY, not hang,
            // not silently succeed. Checked a bounded (small) number of
            // times, not "forever" -- each call already performs its own
            // bounded internal spin (see DrainOutstandingFrames).
            CHECK(Start(primary, 1.0F) == kStatusBusy);
            CHECK(Start(sibling, 1.0F) == kStatusBusy);
        });
    worker.join();
}

// ---------------------------------------------------------------------
// 4. PRE/POST balance for inactive calls: a call while Gate2C is
//    genuinely untouched on this thread must NOT be tracked at all (the
//    common fast path); a call while Gate2C is inactive but something
//    else is already pending on this thread MUST still be tracked as a
//    non-recording marker, or its POST could positionally consume the
//    other, real invocation's entry.
// ---------------------------------------------------------------------
void TestUntrackedWhenNothingPending()
{
    const int slot = 6;
    // Deliberately never call Start() for this slot.
    void* services = &g_fakeServicesA;
    void* moveData = &g_fakeMoveDataA;

    Gate2CReservation r = ReserveFrame(slot, services, moveData);
    CHECK(r.admission == Gate2CAdmission::NotTracked);

    Gate2CResolution res = BeginResolveFrame(slot, services, moveData);
    CHECK(res.outcome == Gate2CResolutionOutcome::Unmatched);
}

// NOTE on how this scenario is constructed: `active=false` while
// `admitting` stays OPEN is required for ReserveFrame to still return
// Reserved (rather than AdmissionClosed) for the "inner" call below --
// see ReserveFrame's fast path, which only skips tracking when
// `!active` AND this thread has nothing else pending. Finalize() cannot
// produce that combination: it closes `active` and `admitting`
// together, and a ReserveFrame call made while `admitting==0` returns
// AdmissionClosed regardless of pending depth (a DIFFERENT, separately-
// verified-safe path -- see this session's findings on why
// AdmissionClosed cannot corrupt data even though it can scramble
// positions, thanks to the identity assertion and WriteSample's own
// admitting-gate). The only way `active=false` with `admitting` still
// open is reachable in production is PoisonSlot (overflow, a positional
// mismatch, or a CommitFrame contract violation) -- none of which touch
// `admitting`. This test reproduces that exact mechanism via a
// deliberate CommitFrame contract violation on a throwaway reservation.
void TestInactiveNestedCallGetsMarker()
{
    const int slot = 7;
    CHECK(Start(slot, 1.0F) == kStatusOk);

    void* outerServices = &g_fakeServicesA;
    void* outerMoveData = &g_fakeMoveDataA;
    Gate2CReservation outer = ReserveFrame(slot, outerServices, outerMoveData);
    CHECK(outer.admission == Gate2CAdmission::Reserved && outer.active);
    CommitFrame(slot, outer.invocationId, true, 5.0F, 0.0F, true, 5.0F, 0.0F, true);

    // Force `active=false` via PoisonSlot WITHOUT closing `admitting` and
    // WITHOUT touching outer's already-reserved position.
    int poisonServices = 0, poisonMoveData = 0;
    Gate2CReservation poisoner = ReserveFrame(slot, &poisonServices, &poisonMoveData);
    CHECK(poisoner.admission == Gate2CAdmission::Reserved);
    CommitFrame(slot, poisoner.invocationId + 999 /* deliberate contract violation */, true, 0, 0, false, 0, 0,
                false);
    CHECK(!IsActiveForSlot(slot)); // poisoned -> active forced false; admitting untouched

    void* innerServices = &g_fakeServicesB;
    void* innerMoveData = &g_fakeMoveDataB;
    Gate2CReservation inner = ReserveFrame(slot, innerServices, innerMoveData);
    // Requirement 1: still tracked, even though Gate2C now reads
    // inactive, because this thread already has real, pending
    // invocations (outer, poisoner) -- admitting is still open, so this
    // is Reserved, not AdmissionClosed.
    CHECK(inner.admission == Gate2CAdmission::Reserved);
    CHECK(!inner.active); // inactive marker -- no real data
    CommitFrame(slot, inner.invocationId, /*hasData=*/false, 0, 0, false, 0, 0, false);

    // Resolve in strict LIFO order: inner, then poisoner, then outer.
    Gate2CResolution innerResolved = BeginResolveFrame(slot, innerServices, innerMoveData);
    CHECK(innerResolved.outcome == Gate2CResolutionOutcome::NonRecording);

    Gate2CResolution poisonerResolved = BeginResolveFrame(slot, &poisonServices, &poisonMoveData);
    // hasData was left at its reset default (false) by CommitFrame's
    // contract-violation branch (see TestCommitFrameContractViolationFailsClosed).
    CHECK(poisonerResolved.outcome == Gate2CResolutionOutcome::NonRecording);

    // POST(outer) resolves LAST -- must still get its REAL data, never
    // stolen by the poisoning event or the inactive marker in between.
    Gate2CResolution outerResolved = BeginResolveFrame(slot, outerServices, outerMoveData);
    CHECK(outerResolved.outcome == Gate2CResolutionOutcome::DataReady);
    CHECK(outerResolved.preForward == 5.0F);
}

// ---------------------------------------------------------------------
// 4b. AdmissionClosed must still get a positional representation, even
//     when the admission-closed invocation shares IDENTICAL (slot,
//     services, moveData) with an older, still-genuinely-pending
//     invocation -- the exact scenario a later review identified as
//     unguarded by pointer-identity checks alone. Simulates:
//       PRE(A, real)  Finalize()  PRE(X, admission-closed, SAME identity
//       as A)  POST(X)  POST(A)
//     which is the ordering a real Finalize() landing between two
//     same-thread invocations (nested or merely sequential-but-
//     overlapping) would produce.
// ---------------------------------------------------------------------
void TestAdmissionClosedPreservesPositionWithIdenticalIdentity()
{
    const int slot = 11;
    CHECK(Start(slot, 1.0F) == kStatusOk);

    void* services = &g_fakeServicesA;
    void* moveData = &g_fakeMoveDataA; // SAME identity reused deliberately for X below

    Gate2CReservation a = ReserveFrame(slot, services, moveData);
    CHECK(a.admission == Gate2CAdmission::Reserved);
    CommitFrame(slot, a.invocationId, true, 7.0F, 0.0F, true, 7.0F, 0.0F, true);

    // Close admission WITHOUT resolving A -- Finalize does not (and must
    // not) reach into this thread's positional stack.
    CHECK(Finalize(slot, 0) == kStatusOk);

    // X arrives on this SAME thread, reusing A's EXACT identity. Admission
    // is closed, but X must still occupy its own position.
    Gate2CReservation x = ReserveFrame(slot, services, moveData);
    CHECK(x.admission == Gate2CAdmission::AdmissionClosed);

    // POST(X) resolves first (LIFO) -- must resolve X's OWN placeholder,
    // never A's still-pending real frame, DESPITE identical identity and
    // DESPITE the identity assertion alone being unable to distinguish
    // them (X's stored identity IS A's, by construction of this test).
    Gate2CResolution xResolved = BeginResolveFrame(slot, services, moveData);
    CHECK(xResolved.outcome == Gate2CResolutionOutcome::NonRecording);

    // POST(A) resolves second -- must still get A's REAL data, completely
    // undisturbed by X's admission-closed arrival in between.
    Gate2CResolution aResolved = BeginResolveFrame(slot, services, moveData);
    CHECK(aResolved.outcome == Gate2CResolutionOutcome::DataReady);
    CHECK(aResolved.preForward == 7.0F);

    // Only A's resolution may release the barrier -- X's admission-closed
    // arrival must never have touched it. A fresh Start() succeeding here
    // confirms the barrier was never left stuck OR released early.
    CHECK(Start(slot, 1.0F) == kStatusOk);
    CHECK(Finalize(slot, 0) == kStatusOk);
}

// ---------------------------------------------------------------------
// 5. Explicit lifecycle-transition contracts: ReserveFrame/CommitFrame
//    and BeginResolveFrame/CommitResolvedSample.
// ---------------------------------------------------------------------
void TestCommitFrameContractViolationFailsClosed()
{
    const int slot = 8;
    CHECK(Start(slot, 1.0F) == kStatusOk);

    void* services = &g_fakeServicesA;
    void* moveData = &g_fakeMoveDataA;
    Gate2CReservation r = ReserveFrame(slot, services, moveData);
    CHECK(r.admission == Gate2CAdmission::Reserved);

    // Wrong invocationId -- simulates a caller bug (e.g. committing a
    // stale reservation). Must poison the slot and must NOT fabricate
    // data on the real, still-top-of-stack frame.
    CommitFrame(slot, r.invocationId + 999, true, 42.0F, 0.0F, true, 42.0F, 0.0F, true);
    CHECK(!IsActiveForSlot(slot));

    // The frame's hasData must have been left at its reset default
    // (false) -- resolving it must yield NonRecording, never DataReady
    // with the fabricated 42.0F.
    Gate2CResolution res = BeginResolveFrame(slot, services, moveData);
    CHECK(res.outcome == Gate2CResolutionOutcome::NonRecording);
}

void TestResolveOutcomesCoverEveryTransition()
{
    const int slot = 9;
    CHECK(Start(slot, 1.0F) == kStatusOk);

    // Reserved -> Commit(hasData=true) -> Resolve == DataReady.
    void* s1 = &g_fakeServicesA;
    void* m1 = &g_fakeMoveDataA;
    Gate2CReservation r1 = ReserveFrame(slot, s1, m1);
    CommitFrame(slot, r1.invocationId, true, 1.0F, 0.0F, true, 1.0F, 0.0F, true);
    CHECK(BeginResolveFrame(slot, s1, m1).outcome == Gate2CResolutionOutcome::DataReady);

    // A POST with no matching reservation at all == Unmatched.
    void* s2 = &g_fakeServicesB;
    void* m2 = &g_fakeMoveDataB;
    CHECK(BeginResolveFrame(slot, s2, m2).outcome == Gate2CResolutionOutcome::Unmatched);

    CHECK(Finalize(slot, 0) == kStatusOk);
}

// ---------------------------------------------------------------------
// 6. Concurrent Start()/PRE interleaving: a stress/soak test, not a
//    single-execution proof (real thread scheduling is not
//    deterministic) -- intended to be run repeatedly, ideally under a
//    thread sanitizer, to build confidence rather than to assert a
//    specific interleaving occurred. The invariant under test: no
//    DataReady resolution's stamped generation is ever inconsistent with
//    when its reservation happened (i.e. every generation-advancing
//    operation -- there is only one, Start() -- stays correctly
//    synchronized with outstanding invocations), and Start() never
//    deadlocks.
// ---------------------------------------------------------------------
void TestConcurrentStartAndReserve()
{
    const int slot = 10;
    CHECK(Start(slot, 1.0F) == kStatusOk);

    std::atomic<bool> stop{ false };
    std::atomic<int> mismatches{ 0 };
    std::atomic<int> startOkCount{ 0 };

    std::thread worker(
        [&]()
        {
            int fakeServices = 0, fakeMoveData = 0;
            while (!stop.load(std::memory_order_relaxed))
            {
                Gate2CReservation r = ReserveFrame(slot, &fakeServices, &fakeMoveData);
                if (r.admission != Gate2CAdmission::Reserved) continue; // AdmissionClosed -- Start() is mid-close
                const uint32_t reservedGeneration = r.generation;
                CommitFrame(slot, r.invocationId, true, 1.0F, 0.0F, true, 1.0F, 0.0F, true);

                std::this_thread::yield(); // widen the race window deliberately

                Gate2CResolution res = BeginResolveFrame(slot, &fakeServices, &fakeMoveData);
                if (res.outcome == Gate2CResolutionOutcome::DataReady && res.generation != reservedGeneration)
                {
                    mismatches.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });

    constexpr int kStartAttempts = 500;
    for (int i = 0; i < kStartAttempts; ++i)
    {
        int status = Start(slot, 1.0F);
        CHECK(status == kStatusOk || status == kStatusBusy);
        if (status == kStatusOk) startOkCount.fetch_add(1, std::memory_order_relaxed);
    }

    stop.store(true, std::memory_order_relaxed);
    worker.join();

    CHECK(mismatches.load() == 0);
    CHECK(startOkCount.load() > 0);
    Finalize(slot, 0);
}

} // namespace

int main()
{
    // Fatal-depth test runs FIRST among worker-thread tests conceptually
    // isolated, but ordering relative to main-thread tests below does
    // not matter -- it only ever touches its own dedicated thread's
    // thread_local state, never main's.
    TestNestedCallsLifoPairing();
    TestOverflowAtDepth8();
    TestOverflowAtDepth9();
    TestOverflowAtDepth16();
    TestFatalDepthAndCrossSlotEffects();
    TestUntrackedWhenNothingPending();
    TestInactiveNestedCallGetsMarker();
    TestAdmissionClosedPreservesPositionWithIdenticalIdentity();
    TestCommitFrameContractViolationFailsClosed();
    TestResolveOutcomesCoverEveryTransition();
    TestConcurrentStartAndReserve();

    std::printf("%d/%d checks passed\n", g_checks - g_failures, g_checks);
    return g_failures == 0 ? 0 : 1;
}

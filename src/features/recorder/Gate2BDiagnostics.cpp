#include "Gate2BDiagnostics.h"
#include "core/log.h"

#include <algorithm>
#include <array>
#include <atomic>

namespace cs2bc::gate2b {
namespace {

constexpr int kMaxSlots = 64;
constexpr uint32_t kActivationWindowCalls = 16;   // first N window-calls per hook => Activation
constexpr uint32_t kCancellationWindowCalls = 16; // first N calls per hook after MarkCancelled => Cancellation
constexpr int64_t kSteadyStateMinIntervalMs = 100; // time-based decimation: at most one
                                                    // SteadyState sample per slot per 100ms,
                                                    // so 24 slots span ~2.4s regardless of the
                                                    // actual hook call rate (fixes the earlier
                                                    // call-count-based stride's implicit
                                                    // assumption of a constant call rate).
constexpr size_t kActivationCap = 16;
constexpr size_t kSteadyCap = 24;
constexpr size_t kCancelCap = 16;
constexpr size_t kPostCap = 16;
constexpr int kMaxDrainIterations = 100000; // bounded, not unbounded: a writer's critical
                                             // section is a handful of POD copies, expected
                                             // to complete in well under this many iterations;
                                             // this caps worst-case spin time, it does not
                                             // wait on wall-clock time or an OS primitive.

enum class Phase : uint8_t { None = 0, Activation, SteadyState, Cancellation, PostCancel };

const char* PhaseName(Phase p)
{
    switch (p)
    {
    case Phase::Activation: return "Activation";
    case Phase::SteadyState: return "SteadyState";
    case Phase::Cancellation: return "Cancellation";
    case Phase::PostCancel: return "PostCancel";
    default: return "None";
    }
}

bool ValidSlot(int slot) { return slot >= 0 && slot < kMaxSlots; }

struct PrcSample
{
    uint32_t windowSeq = 0;
    Phase phase = Phase::None;
    uint32_t physicsSimulateSeq = 0; // coarse same-slot hint only, NOT a verified frame/tick
                                      // correlation -- see the header comment above
                                      // RecordPhysicsSimulateCall for the exact, deliberately
                                      // narrow meaning of this field
    int cmdNum = 0;
    bool hasUsercmdMovementObserved = false;
    bool hasBase = false; // false => fwd/side/subtick below are placeholders, not observations
    float fwdBefore = 0, sideBefore = 0, fwdAfter = 0, sideAfter = 0;
    int subtickCountBefore = 0;
};

struct PmSample
{
    uint32_t windowSeq = 0;
    Phase phase = Phase::None;
    uint32_t physicsSimulateSeq = 0;
    float forwardMove = 0, sideMove = 0, velX = 0, velY = 0, velZ = 0;
};

// Count fields are atomic and claimed via fetch_add (see ClaimSlot below),
// not plain uint32_t: WriterGuard/inFlight only serialize writers against
// DiagnosticFinalize, they do nothing to serialize two genuinely concurrent
// writers against EACH OTHER. A plain "if (count < cap) buf[count++] = s"
// is an unsynchronized read-modify-write on count and a possible same-index
// write on buf[] if two writers ever race here -- a real data race. Each
// count is therefore claimed atomically first; a writer only ever touches
// the array slot it uniquely claimed, and a claim landing at or past the
// capacity is dropped (never written), never blocking gameplay.
struct PrcBuffers
{
    std::array<PrcSample, kActivationCap> activation{};
    std::atomic<uint32_t> activationCount{ 0 };
    std::array<PrcSample, kSteadyCap> steady{};
    std::atomic<uint32_t> steadyCount{ 0 };
    std::array<PrcSample, kCancelCap> cancellation{};
    std::atomic<uint32_t> cancellationCount{ 0 };
    std::array<PrcSample, kPostCap> postCancel{};
    std::atomic<uint32_t> postCancelCount{ 0 };

    // Only callable once DiagnosticStart has already confirmed (via
    // DrainWriters) that no writer can be active -- never assigns over a
    // struct containing atomics, just resets each counter individually.
    void Reset()
    {
        activationCount.store(0, std::memory_order_relaxed);
        steadyCount.store(0, std::memory_order_relaxed);
        cancellationCount.store(0, std::memory_order_relaxed);
        postCancelCount.store(0, std::memory_order_relaxed);
    }
};

struct PmBuffers
{
    std::array<PmSample, kActivationCap> activation{};
    std::atomic<uint32_t> activationCount{ 0 };
    std::array<PmSample, kSteadyCap> steady{};
    std::atomic<uint32_t> steadyCount{ 0 };
    std::array<PmSample, kCancelCap> cancellation{};
    std::atomic<uint32_t> cancellationCount{ 0 };
    std::array<PmSample, kPostCap> postCancel{};
    std::atomic<uint32_t> postCancelCount{ 0 };

    void Reset()
    {
        activationCount.store(0, std::memory_order_relaxed);
        steadyCount.store(0, std::memory_order_relaxed);
        cancellationCount.store(0, std::memory_order_relaxed);
        postCancelCount.store(0, std::memory_order_relaxed);
    }
};

// Atomically claims a unique slot index for a bounded array of capacity
// `cap`. Returns true and sets *outIdx to a valid, exclusively-owned index
// if one was available; returns false (and increments *droppedCounter) if
// the claim landed at or past capacity -- the caller must then not write
// to the array at all. Two concurrent callers can never receive the same
// index, so the resulting array write (by whichever one succeeds) never
// aliases another writer's write.
bool ClaimSlot(std::atomic<uint32_t>& counter, size_t cap, uint32_t& outIdx)
{
    uint32_t idx = counter.fetch_add(1, std::memory_order_relaxed);
    if (idx < cap)
    {
        outIdx = idx;
        return true;
    }
    return false;
}

// Writer-drain protocol state for one slot. Read the block comment above
// DiagnosticFinalize for the full race analysis (races A-F).
struct SlotState
{
    // admitting: 1 while new writers may register and proceed to write;
    // set to 0 by DiagnosticStart/DiagnosticFinalize before either of them
    // touches the buffers. Writers check this AFTER registering (see
    // RegisterWriter below), not before, which is what closes race B.
    std::atomic<uint32_t> admitting{ 0 };
    // inFlight: count of writers currently between registration and release.
    // DiagnosticStart/Finalize spin (bounded) until this reads 0 after they
    // have already set admitting=0, which is what closes races A and C.
    std::atomic<int32_t> inFlight{ 0 };
    std::atomic<bool> finalized{ false };
    std::atomic<bool> abortedFlag{ false };
    // Explicit cancellation flag, separate from any counter value, so a mark
    // taken at prcWindowCalls==0 is never confused with "not yet marked".
    std::atomic<bool> cancelled{ false };

    std::atomic<uint32_t> prcTotal{ 0 };
    std::atomic<uint32_t> prcNullCmd{ 0 };
    std::atomic<uint32_t> prcCaptured{ 0 };
    std::atomic<uint32_t> prcDropped{ 0 };
    std::atomic<uint32_t> prcUnsampled{ 0 };
    std::atomic<uint32_t> prcWindowCalls{ 0 };
    std::atomic<uint32_t> prcCallsAtCancelMark{ 0 };
    std::atomic<int64_t> prcSteadyLastSampleMs{ 0 };

    std::atomic<uint32_t> pmTotal{ 0 };
    std::atomic<uint32_t> pmCaptured{ 0 };
    std::atomic<uint32_t> pmDropped{ 0 };
    std::atomic<uint32_t> pmUnsampled{ 0 };
    std::atomic<uint32_t> pmWindowCalls{ 0 };
    std::atomic<uint32_t> pmCallsAtCancelMark{ 0 };
    std::atomic<int64_t> pmSteadyLastSampleMs{ 0 };

    std::atomic<uint32_t> physicsSimulateCallSeq{ 0 };

    // Snapshot of the GLOBAL slot-resolution-failure counters taken at
    // DiagnosticStart, so EmitSummary can report a window-scoped delta
    // instead of the raw lifetime total. See g_prcSlotResolutionFailed below.
    std::atomic<uint32_t> prcSlotResolutionFailedBaseline{ 0 };
    std::atomic<uint32_t> pmSlotResolutionFailedBaseline{ 0 };
};

std::array<SlotState, kMaxSlots> g_state{};
std::array<PrcBuffers, kMaxSlots> g_prcBuf{};
std::array<PmBuffers, kMaxSlots> g_pmBuf{};

// Calls observed with slot<0 (resolution failed before a slot even existed
// to index these per-slot arrays). Global and lifetime-scoped by
// construction (there is no per-slot state to reset when slot<0); reported
// as a delta against each slot's own DiagnosticStart-time baseline, labeled
// "GLOBAL WINDOW DELTA" in the summary -- never presented as if it were a
// per-slot lifetime count.
std::atomic<uint32_t> g_prcSlotResolutionFailed{ 0 };
std::atomic<uint32_t> g_pmSlotResolutionFailed{ 0 };

// RAII writer registration. Construction registers (increments inFlight)
// BEFORE any eligibility check is made by the caller; destruction releases
// on every exit path unconditionally, including early returns, which is
// what guarantees requirement "every exit path must release its writer
// registration" even as this file's write logic evolves.
class WriterGuard
{
  public:
    explicit WriterGuard(std::atomic<int32_t>& counter) : m_counter(counter)
    {
        m_counter.fetch_add(1, std::memory_order_seq_cst);
    }
    ~WriterGuard() { m_counter.fetch_sub(1, std::memory_order_seq_cst); }
    WriterGuard(const WriterGuard&) = delete;
    WriterGuard& operator=(const WriterGuard&) = delete;

  private:
    std::atomic<int32_t>& m_counter;
};

// Bounded drain: waits for admitted writers to release. Must only be called
// after the caller has already set admitting=0. Returns true if inFlight
// reached 0 within the bounded iteration cap; false if it did not (caller
// must then NOT touch the buffers). This is a tight, bounded spin -- not an
// OS-level wait, not blocking, not unbounded.
//
// Memory order: admitting and inFlight are two SEPARATE atomic objects.
// Acquire/release on each individually orders THAT object's own writes
// relative to a reader who observes a specific value, but does not, by
// itself, guarantee that a third thread's read of admitting will observe
// Finalize's store merely because it happened earlier in wall-clock time --
// that requires either a proven synchronizes-with chain or a stronger
// ordering. Rather than lean on this platform's TSO behavior, every
// operation on admitting and inFlight uses memory_order_seq_cst, which
// establishes one total order all threads agree on across BOTH variables.
// This is the standard, provably-correct tool for exactly this class of
// "ordering observed by a third party across multiple atomics" concern, and
// is cheap enough at this call frequency (per-hook-call, not a hot inner
// loop) that the extra cost is not a meaningful tradeoff against certainty.
bool DrainWriters(SlotState& st)
{
    for (int i = 0; i < kMaxDrainIterations; ++i)
    {
        if (st.inFlight.load(std::memory_order_seq_cst) == 0) return true;
    }
    return st.inFlight.load(std::memory_order_seq_cst) == 0;
}

void LogPrcSample(int slot, const char* bucket, const PrcSample& s)
{
    if (!s.hasBase)
    {
        BC_LOG_INFO(
            "[gate2b][slot=%d][prc][%s] w=%u phase=%s pmSeq=%u cmdNum=%d hasUsercmdMovement=%d hasBase=0 "
            "(no base submessage -- fwd/side/subtick below are NOT observations)\n",
            slot, bucket, s.windowSeq, PhaseName(s.phase), s.physicsSimulateSeq, s.cmdNum,
            s.hasUsercmdMovementObserved ? 1 : 0);
        return;
    }
    BC_LOG_INFO(
        "[gate2b][slot=%d][prc][%s] w=%u phase=%s pmSeq=%u cmdNum=%d hasUsercmdMovement=%d hasBase=1 "
        "fwd=%.4f->%.4f side=%.4f->%.4f subtickBefore=%d\n",
        slot, bucket, s.windowSeq, PhaseName(s.phase), s.physicsSimulateSeq, s.cmdNum,
        s.hasUsercmdMovementObserved ? 1 : 0, s.fwdBefore, s.fwdAfter, s.sideBefore, s.sideAfter,
        s.subtickCountBefore);
}

void LogPmSample(int slot, const char* bucket, const PmSample& s)
{
    BC_LOG_INFO(
        "[gate2b][slot=%d][pm][%s] w=%u phase=%s pmSeq=%u forward=%.4f side=%.4f vel=(%.4f,%.4f,%.4f)\n",
        slot, bucket, s.windowSeq, PhaseName(s.phase), s.physicsSimulateSeq, s.forwardMove, s.sideMove, s.velX,
        s.velY, s.velZ);
}

// Only called after the caller has confirmed (via DrainWriters) that no
// writer can be mid-write. Read-only with respect to the buffers.
void EmitSummary(int slot, bool aborted)
{
    const SlotState& st = g_state[slot];
    const PrcBuffers& pb = g_prcBuf[slot];
    const PmBuffers& mb = g_pmBuf[slot];

    // Claimed counts can overshoot their cap under contention (fetch_add
    // keeps incrementing past it); clamp to the actual array capacity before
    // iterating or reporting "reached" -- the excess is already accounted
    // for in prcDropped/pmDropped, not lost, just not indexed into here.
    const uint32_t pbActivationN = std::min<uint32_t>(pb.activationCount.load(std::memory_order_relaxed), kActivationCap);
    const uint32_t pbSteadyN = std::min<uint32_t>(pb.steadyCount.load(std::memory_order_relaxed), kSteadyCap);
    const uint32_t pbCancelN = std::min<uint32_t>(pb.cancellationCount.load(std::memory_order_relaxed), kCancelCap);
    const uint32_t pbPostN = std::min<uint32_t>(pb.postCancelCount.load(std::memory_order_relaxed), kPostCap);
    const uint32_t mbActivationN = std::min<uint32_t>(mb.activationCount.load(std::memory_order_relaxed), kActivationCap);
    const uint32_t mbSteadyN = std::min<uint32_t>(mb.steadyCount.load(std::memory_order_relaxed), kSteadyCap);
    const uint32_t mbCancelN = std::min<uint32_t>(mb.cancellationCount.load(std::memory_order_relaxed), kCancelCap);
    const uint32_t mbPostN = std::min<uint32_t>(mb.postCancelCount.load(std::memory_order_relaxed), kPostCap);

    const bool reachedSteady = pbSteadyN > 0 || mbSteadyN > 0;
    const bool reachedActivation = pbActivationN > 0 || mbActivationN > 0;
    const char* result = aborted ? "ABORTED" : ((reachedActivation && reachedSteady && st.finalized.load()) ? "COMPLETE" : "PARTIAL");
    // NOTE: st.finalized is read here only for labeling purposes after the
    // caller has already decided the outcome; it does not gate this read.

    uint32_t prcGlobalDelta = g_prcSlotResolutionFailed.load(std::memory_order_relaxed) -
                               st.prcSlotResolutionFailedBaseline.load(std::memory_order_relaxed);
    uint32_t pmGlobalDelta = g_pmSlotResolutionFailed.load(std::memory_order_relaxed) -
                              st.pmSlotResolutionFailedBaseline.load(std::memory_order_relaxed);

    BC_LOG_INFO("[gate2b][slot=%d] ==== FINALIZE RESULT=%s ====\n", slot, result);
    BC_LOG_INFO(
        "[gate2b][slot=%d] prc: total=%u nullCmd=%u captured=%u dropped=%u unsampled=%u "
        "slotResolutionFailed[GLOBAL WINDOW DELTA]=%u\n",
        slot, st.prcTotal.load(), st.prcNullCmd.load(), st.prcCaptured.load(), st.prcDropped.load(),
        st.prcUnsampled.load(), prcGlobalDelta);
    BC_LOG_INFO(
        "[gate2b][slot=%d] pm:  total=%u captured=%u dropped=%u unsampled=%u "
        "slotResolutionFailed[GLOBAL WINDOW DELTA]=%u\n",
        slot, st.pmTotal.load(), st.pmCaptured.load(), st.pmDropped.load(), st.pmUnsampled.load(), pmGlobalDelta);

    for (uint32_t i = 0; i < pbActivationN; ++i) LogPrcSample(slot, "Activation", pb.activation[i]);
    for (uint32_t i = 0; i < pbSteadyN; ++i) LogPrcSample(slot, "SteadyState", pb.steady[i]);
    for (uint32_t i = 0; i < pbCancelN; ++i) LogPrcSample(slot, "Cancellation", pb.cancellation[i]);
    for (uint32_t i = 0; i < pbPostN; ++i) LogPrcSample(slot, "PostCancel", pb.postCancel[i]);

    for (uint32_t i = 0; i < mbActivationN; ++i) LogPmSample(slot, "Activation", mb.activation[i]);
    for (uint32_t i = 0; i < mbSteadyN; ++i) LogPmSample(slot, "SteadyState", mb.steady[i]);
    for (uint32_t i = 0; i < mbCancelN; ++i) LogPmSample(slot, "Cancellation", mb.cancellation[i]);
    for (uint32_t i = 0; i < mbPostN; ++i) LogPmSample(slot, "PostCancel", mb.postCancel[i]);

    BC_LOG_INFO("[gate2b][slot=%d] ==== END ====\n", slot);
}

} // namespace

int DiagnosticStart(int slot)
{
    if (!ValidSlot(slot)) return kStatusInvalidSlot;
    SlotState& st = g_state[slot];

    // Close admission for whatever window may still be open, then drain any
    // writer that was already admitted before we reset (race D). If the
    // drain does not complete in time, do NOT reset -- leave the previous
    // window's buffers untouched and report BUSY so the caller can retry.
    st.admitting.store(0, std::memory_order_seq_cst);
    if (!DrainWriters(st)) return kStatusBusy;

    st.finalized.store(false, std::memory_order_relaxed);
    st.abortedFlag.store(false, std::memory_order_relaxed);
    st.cancelled.store(false, std::memory_order_relaxed);
    st.prcTotal.store(0, std::memory_order_relaxed);
    st.prcNullCmd.store(0, std::memory_order_relaxed);
    st.prcCaptured.store(0, std::memory_order_relaxed);
    st.prcDropped.store(0, std::memory_order_relaxed);
    st.prcUnsampled.store(0, std::memory_order_relaxed);
    st.prcWindowCalls.store(0, std::memory_order_relaxed);
    st.prcCallsAtCancelMark.store(0, std::memory_order_relaxed);
    st.prcSteadyLastSampleMs.store(0, std::memory_order_relaxed);
    st.pmTotal.store(0, std::memory_order_relaxed);
    st.pmCaptured.store(0, std::memory_order_relaxed);
    st.pmDropped.store(0, std::memory_order_relaxed);
    st.pmUnsampled.store(0, std::memory_order_relaxed);
    st.pmWindowCalls.store(0, std::memory_order_relaxed);
    st.pmCallsAtCancelMark.store(0, std::memory_order_relaxed);
    st.pmSteadyLastSampleMs.store(0, std::memory_order_relaxed);
    st.physicsSimulateCallSeq.store(0, std::memory_order_relaxed);
    st.prcSlotResolutionFailedBaseline.store(g_prcSlotResolutionFailed.load(std::memory_order_relaxed),
                                              std::memory_order_relaxed);
    st.pmSlotResolutionFailedBaseline.store(g_pmSlotResolutionFailed.load(std::memory_order_relaxed),
                                             std::memory_order_relaxed);
    g_prcBuf[slot].Reset(); // PrcBuffers/PmBuffers contain atomics now, so they
    g_pmBuf[slot].Reset();  // cannot be assigned wholesale -- reset each counter

    st.admitting.store(1, std::memory_order_seq_cst); // reopen admission for the NEW window
    return kStatusOk;
}

void DiagnosticMarkCancelled(int slot)
{
    if (!ValidSlot(slot)) return;
    SlotState& st = g_state[slot];
    if (st.admitting.load(std::memory_order_seq_cst) == 0) return; // no active window
    if (st.cancelled.exchange(true, std::memory_order_acq_rel)) return; // already marked; idempotent no-op
    st.prcCallsAtCancelMark.store(st.prcWindowCalls.load(std::memory_order_relaxed), std::memory_order_relaxed);
    st.pmCallsAtCancelMark.store(st.pmWindowCalls.load(std::memory_order_relaxed), std::memory_order_relaxed);
}

// ---- Writer-drain protocol for DiagnosticFinalize ----
//
// admitting and inFlight are two SEPARATE atomic objects. Using acquire and
// release individually on each is not, by itself, sufficient: it does not
// establish that a third writer's read of admitting will observe Finalize's
// store merely because that store happened earlier in wall-clock time --
// that specific guarantee requires either a proven synchronizes-with chain
// or a memory order that provides a single total order across BOTH
// variables. Every operation on admitting and inFlight therefore uses
// memory_order_seq_cst (see WriterGuard and DrainWriters above), which
// gives exactly that: one total order every thread agrees on for these two
// variables together, independent of any platform-specific store-buffer
// behavior. The counters below this (prcCaptured, prcDropped, etc.) remain
// memory_order_relaxed deliberately -- they do not need this cross-variable
// guarantee, since nothing else's correctness depends on their ordering
// relative to admitting/inFlight, only their own final values once the
// drain above has already established quiescence.
//
// Race analysis (A-F as specified):
//  A. Writer registers immediately before finalization begins: the writer's
//     WriterGuard increments inFlight before Finalize runs at all, so
//     Finalize's DrainWriters call (which happens strictly after admitting
//     is set to 0) will observe inFlight!=0 and wait for it -- safe.
//  B. Writer observes admitting==true immediately before finalization:
//     every writer registers (increments inFlight) BEFORE it ever reads
//     admitting, so there is no code path where a writer reads admitting
//     without already being counted in inFlight; combined with seq_cst's
//     single total order across both variables, a writer whose read of
//     admitting is positioned (in that total order) after Finalize's
//     store is guaranteed to observe the closed value -- safe.
//  C. Finalization begins while a writer is mid-copy: the writer is already
//     registered (inFlight>0); Finalize's DrainWriters spins until the
//     writer's WriterGuard destructor decrements it, which happens-after
//     the writer's buffer write in program order and is ordered against
//     Finalize's seq_cst load in DrainWriters by the same total order --
//     safe.
//  D. DiagnosticStart runs while a previous capture is still draining: Start
//     performs the exact same close-admission + DrainWriters sequence as
//     Finalize before it resets anything; if the drain fails, Start returns
//     kStatusBusy and leaves the old buffers untouched -- safe.
//  E. DiagnosticFinalize is called twice: the first call's
//     finalized.exchange(true) wins; the second sees finalized==true after
//     its own (harmless, already-satisfied) admitting/drain steps and just
//     re-emits the frozen snapshot -- safe, idempotent.
//  F. An abort races with normal completion: both calls perform the same
//     close+drain; exactly one wins the finalized.exchange and sets
//     abortedFlag; the loser re-emits using the WINNER's abortedFlag, so the
//     native summary's own COMPLETE/ABORTED label is always self-consistent
//     regardless of which caller "wins" -- safe at the native layer (the C#
//     harness needs its own single-owner guard for its half of this, see
//     PocHarnessPlugin.cs).
//
// Timeout behavior: if DrainWriters returns false (bounded cap exhausted
// without observing inFlight==0), both DiagnosticStart and DiagnosticFinalize
// return kStatusBusy immediately, before touching finalized, abortedFlag, or
// either buffer array -- no read, reset, or destruction of a potentially
// still-active buffer occurs on that path.
int DiagnosticFinalize(int slot, int aborted)
{
    if (!ValidSlot(slot)) return kStatusInvalidSlot;
    SlotState& st = g_state[slot];

    st.admitting.store(0, std::memory_order_seq_cst); // close admission first, always
    if (!DrainWriters(st)) return kStatusBusy;         // could not confirm quiescence -- do
                                                        // NOT read/reset; admission stays
                                                        // closed, so no new samples are lost
                                                        // and the caller may safely retry.

    if (st.finalized.exchange(true, std::memory_order_acq_rel))
    {
        EmitSummary(slot, st.abortedFlag.load(std::memory_order_acquire));
        return kStatusRepeat;
    }
    st.abortedFlag.store(aborted != 0, std::memory_order_release);
    EmitSummary(slot, aborted != 0);
    return kStatusOk;
}

void RecordPlayerRunCommandEntry(int slot, bool hadCmd)
{
    if (!ValidSlot(slot))
    {
        g_prcSlotResolutionFailed.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    SlotState& st = g_state[slot];
    st.prcTotal.fetch_add(1, std::memory_order_relaxed);
    if (!hadCmd) st.prcNullCmd.fetch_add(1, std::memory_order_relaxed);
}

void RecordProcessMovementEntry(int slot)
{
    if (!ValidSlot(slot))
    {
        g_pmSlotResolutionFailed.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    g_state[slot].pmTotal.fetch_add(1, std::memory_order_relaxed);
}

void RecordPhysicsSimulateCall(int slot)
{
    if (!ValidSlot(slot)) return;
    g_state[slot].physicsSimulateCallSeq.fetch_add(1, std::memory_order_relaxed);
}

uint32_t CurrentPhysicsSimulateSeq(int slot)
{
    if (!ValidSlot(slot)) return 0;
    return g_state[slot].physicsSimulateCallSeq.load(std::memory_order_relaxed);
}

void RecordPlayerRunCommandObservation(int slot, uint32_t physicsSimulateSeq, int64_t nowMs, int cmdNum,
                                        bool hasUsercmdMovementObserved, bool hasBase, float fwdBefore,
                                        float sideBefore, float fwdAfter, float sideAfter, int subtickCountBefore)
{
    if (!ValidSlot(slot)) return;
    SlotState& st = g_state[slot];

    WriterGuard guard(st.inFlight); // register BEFORE checking eligibility (closes race B)
    if (st.admitting.load(std::memory_order_seq_cst) == 0)
    {
        st.prcUnsampled.fetch_add(1, std::memory_order_relaxed); // capture not open; not an error
        return; // guard releases here
    }

    uint32_t w = st.prcWindowCalls.fetch_add(1, std::memory_order_relaxed) + 1;
    bool cancelled = st.cancelled.load(std::memory_order_acquire);

    Phase phase;
    if (!cancelled)
    {
        phase = (w <= kActivationWindowCalls) ? Phase::Activation : Phase::SteadyState;
    }
    else
    {
        uint32_t mark = st.prcCallsAtCancelMark.load(std::memory_order_relaxed);
        uint32_t sinceCancel = (w > mark) ? (w - mark) : 0;
        phase = (sinceCancel <= kCancellationWindowCalls) ? Phase::Cancellation : Phase::PostCancel;
    }

    PrcSample s{ w, phase, physicsSimulateSeq, cmdNum, hasUsercmdMovementObserved, hasBase,
                 fwdBefore, sideBefore, fwdAfter, sideAfter, subtickCountBefore };
    PrcBuffers& buf = g_prcBuf[slot];
    uint32_t idx = 0;
    switch (phase)
    {
    case Phase::Activation:
        if (ClaimSlot(buf.activationCount, kActivationCap, idx)) { buf.activation[idx] = s; st.prcCaptured.fetch_add(1); }
        else st.prcDropped.fetch_add(1);
        break;
    case Phase::SteadyState:
    {
        int64_t prev = st.prcSteadyLastSampleMs.load(std::memory_order_relaxed);
        if (nowMs - prev >= kSteadyStateMinIntervalMs &&
            st.prcSteadyLastSampleMs.compare_exchange_strong(prev, nowMs, std::memory_order_relaxed))
        {
            if (ClaimSlot(buf.steadyCount, kSteadyCap, idx)) { buf.steady[idx] = s; st.prcCaptured.fetch_add(1); }
            else st.prcDropped.fetch_add(1);
        }
        else
        {
            st.prcUnsampled.fetch_add(1); // deliberately decimated by time, not a full-buffer drop
        }
        break;
    }
    case Phase::Cancellation:
        if (ClaimSlot(buf.cancellationCount, kCancelCap, idx)) { buf.cancellation[idx] = s; st.prcCaptured.fetch_add(1); }
        else st.prcDropped.fetch_add(1);
        break;
    case Phase::PostCancel:
        if (ClaimSlot(buf.postCancelCount, kPostCap, idx)) { buf.postCancel[idx] = s; st.prcCaptured.fetch_add(1); }
        else st.prcDropped.fetch_add(1);
        break;
    default:
        break;
    }
} // guard releases here (or at any earlier return above)

void RecordProcessMovementObservation(int slot, uint32_t physicsSimulateSeq, int64_t nowMs, float forwardMove,
                                       float sideMove, float velX, float velY, float velZ)
{
    if (!ValidSlot(slot)) return;
    SlotState& st = g_state[slot];

    WriterGuard guard(st.inFlight);
    if (st.admitting.load(std::memory_order_seq_cst) == 0)
    {
        st.pmUnsampled.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    uint32_t w = st.pmWindowCalls.fetch_add(1, std::memory_order_relaxed) + 1;
    bool cancelled = st.cancelled.load(std::memory_order_acquire);

    Phase phase;
    if (!cancelled)
    {
        phase = (w <= kActivationWindowCalls) ? Phase::Activation : Phase::SteadyState;
    }
    else
    {
        uint32_t mark = st.pmCallsAtCancelMark.load(std::memory_order_relaxed);
        uint32_t sinceCancel = (w > mark) ? (w - mark) : 0;
        phase = (sinceCancel <= kCancellationWindowCalls) ? Phase::Cancellation : Phase::PostCancel;
    }

    PmSample s{ w, phase, physicsSimulateSeq, forwardMove, sideMove, velX, velY, velZ };
    PmBuffers& buf = g_pmBuf[slot];
    uint32_t idx = 0;
    switch (phase)
    {
    case Phase::Activation:
        if (ClaimSlot(buf.activationCount, kActivationCap, idx)) { buf.activation[idx] = s; st.pmCaptured.fetch_add(1); }
        else st.pmDropped.fetch_add(1);
        break;
    case Phase::SteadyState:
    {
        int64_t prev = st.pmSteadyLastSampleMs.load(std::memory_order_relaxed);
        if (nowMs - prev >= kSteadyStateMinIntervalMs &&
            st.pmSteadyLastSampleMs.compare_exchange_strong(prev, nowMs, std::memory_order_relaxed))
        {
            if (ClaimSlot(buf.steadyCount, kSteadyCap, idx)) { buf.steady[idx] = s; st.pmCaptured.fetch_add(1); }
            else st.pmDropped.fetch_add(1);
        }
        else
        {
            st.pmUnsampled.fetch_add(1);
        }
        break;
    }
    case Phase::Cancellation:
        if (ClaimSlot(buf.cancellationCount, kCancelCap, idx)) { buf.cancellation[idx] = s; st.pmCaptured.fetch_add(1); }
        else st.pmDropped.fetch_add(1);
        break;
    case Phase::PostCancel:
        if (ClaimSlot(buf.postCancelCount, kPostCap, idx)) { buf.postCancel[idx] = s; st.pmCaptured.fetch_add(1); }
        else st.pmDropped.fetch_add(1);
        break;
    default:
        break;
    }
}

} // namespace cs2bc::gate2b

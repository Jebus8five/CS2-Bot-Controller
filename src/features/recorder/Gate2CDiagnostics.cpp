#include "Gate2CDiagnostics.h"
#include "core/log.h"

#include <algorithm>
#include <array>
#include <atomic>

namespace cs2bc::gate2c {
namespace {

constexpr int kMaxSlots = 64;
constexpr uint32_t kActivationWindowCalls = 16;
constexpr uint32_t kCancellationWindowCalls = 16;
constexpr int64_t kSteadyStateMinIntervalMs = 100;
constexpr size_t kActivationCap = 16;
constexpr size_t kSteadyCap = 24;
constexpr size_t kCancelCap = 16;
constexpr size_t kPostCap = 16;
constexpr int kMaxDrainIterations = 100000; // bounded spin cap, not a wall-clock/OS wait --
                                             // see Gate2BDiagnostics.cpp's DrainWriters comment
                                             // for the full rationale; identical reasoning here.

// Bounded-tier capacity: real nesting depth (true recursion into
// ProcessMovement) is expected to be 1, rarely 2. 8 is a generous margin
// -- see this file's header comment for how this value now also has to
// absorb backlog from a slow/delayed (not yet lost) POST, not just true
// recursion depth.
constexpr int kGate2CMaxFrameDepth = 8;
// Overflow-tier capacity: same-thread invocations beyond the bounded
// tier still get a position (just `slot`, no payload) so their eventual
// POST cannot fall through to consume an unrelated entry. Fixed size,
// never grows -- see ReserveFrame's fatal-depth path for what happens if
// even this is exhausted.
constexpr int kGate2COverflowCap = 8;

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

// One complete, POSITIONALLY-RESOLVED PRE+INJECTED+POST observation for a
// single ProcessMovement invocation. Written exactly once (from
// ResolveFrame, via WriteSample) after POST has positionally resolved
// the bounded-tier entry ReserveFrame/CommitFrame prepared for it, and
// the secondary (slot, services, moveData) assertion confirmed the
// positional invariant held for this entry.
struct Gate2CSample
{
    uint32_t windowSeq = 0;
    Phase phase = Phase::None;
    uint64_t invocationId = 0;
    uint32_t generation = 0;
    float writeScale = 0;
    bool wroteIntent = false; // false: no active movement intent existed for this
                              // invocation (e.g. already cancelled) -- PRE/POST are
                              // still real observations, INJECTED is not meaningful.
    float preForward = 0, preSide = 0;
    float injectedForward = 0, injectedSide = 0;
    float postForward = 0, postSide = 0;
    float postVelX = 0, postVelY = 0, postVelZ = 0;
    float postOriginX = 0, postOriginY = 0, postOriginZ = 0;
    bool lockAllActive = false;
};

struct Gate2CBuffers
{
    std::array<Gate2CSample, kActivationCap> activation{};
    std::atomic<uint32_t> activationCount{ 0 };
    std::array<Gate2CSample, kSteadyCap> steady{};
    std::atomic<uint32_t> steadyCount{ 0 };
    std::array<Gate2CSample, kCancelCap> cancellation{};
    std::atomic<uint32_t> cancellationCount{ 0 };
    std::array<Gate2CSample, kPostCap> postCancel{};
    std::atomic<uint32_t> postCancelCount{ 0 };

    // Only safe to call once the caller has already confirmed (via
    // DrainWriters) that no writer can be active -- resets each atomic
    // counter individually rather than assigning over the struct.
    void Reset()
    {
        activationCount.store(0, std::memory_order_relaxed);
        steadyCount.store(0, std::memory_order_relaxed);
        cancellationCount.store(0, std::memory_order_relaxed);
        postCancelCount.store(0, std::memory_order_relaxed);
    }
};

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

struct SlotState
{
    // Start..Finalize gate. Read by IsActiveForSlot on every PRE hook
    // call (via ReserveFrame); this decides whether a new invocation
    // gets real diagnostic data. The POST hook does NOT read this -- see
    // `generation` below for why it doesn't need to.
    std::atomic<bool> active{ false };
    std::atomic<float> writeScale{ 0.0F };

    // Advanced once per successful Start. A frame reserved under
    // generation G is only ever committed to the buffers if this still
    // equals G at the moment its matching POST arrives -- this is the
    // entire mechanism that prevents a POST from an earlier test landing
    // in a later one, without needing to reach into any thread's
    // thread_local stack from Start/Finalize.
    std::atomic<uint32_t> generation{ 0 };

    // Same writer-drain protocol as Gate2BDiagnostics.cpp (admitting +
    // inFlight, both memory_order_seq_cst throughout, for the same
    // single-total-order-across-two-atomics reason documented there).
    // This protects the SHARED buffer/claim state below, not the
    // per-thread frame stack (which needs no cross-thread protection --
    // each thread only ever touches its own copy).
    std::atomic<uint32_t> admitting{ 0 };
    std::atomic<int32_t> inFlight{ 0 };
    std::atomic<bool> finalized{ false };
    std::atomic<bool> abortedFlag{ false };
    std::atomic<bool> cancelled{ false };

    std::atomic<uint32_t> windowCalls{ 0 };
    std::atomic<uint32_t> callsAtCancelMark{ 0 };
    std::atomic<int64_t> steadyLastSampleMs{ 0 };
    std::atomic<uint32_t> captured{ 0 };
    std::atomic<uint32_t> dropped{ 0 };

    // Pairing-anomaly counters, each reported distinctly in the summary
    // rather than folded into a generic "dropped" bucket.
    std::atomic<uint32_t> overflow{ 0 };
    std::atomic<uint32_t> unmatchedPost{ 0 };
    std::atomic<uint32_t> staleGeneration{ 0 };
    std::atomic<uint32_t> nonRecordingMatch{ 0 };   // correct pairing, no data -- see header comment
    std::atomic<uint32_t> positionalMismatch{ 0 };  // positional pop's stored identity disagreed with
                                                     // this POST's own -- see ResolveFrame's comment
    std::atomic<uint32_t> fatalDepth{ 0 };           // this thread exhausted BOTH bounded tiers -- see
                                                      // ReserveFrame's comment
    std::atomic<uint32_t> admissionClosedTracked{ 0 }; // benign: an admission-closed invocation still
                                                        // consumed a tier position (see ReserveFrame) --
                                                        // never poisons, purely observational

    // Set whenever this generation's pairing correctness could no longer
    // be guaranteed (overflow, positional mismatch, or fatal depth);
    // once true, EmitSummary reports this generation's result as INVALID
    // regardless of captured/dropped counts.
    std::atomic<bool> poisoned{ false };

    // Lifecycle barrier: count of Gate2C invocations reserved
    // (ReserveFrame) but not yet positionally resolved (ResolveFrame)
    // for this slot, across ALL threads. Start() will not advance the
    // generation while this is nonzero -- see Start()'s own comment for
    // the exact hazard this closes. Decremented ONLY when ResolveFrame
    // positionally resolves the entry that reserved it (bounded-tier pop
    // or overflow-tier pop) -- NEVER on a discard/refusal/fatal-depth
    // decline. seq_cst throughout for the same single-total-order-
    // across-threads reason as admitting/inFlight above.
    std::atomic<int32_t> outstandingFrames{ 0 };
};

std::array<SlotState, kMaxSlots> g_state{};
std::array<Gate2CBuffers, kMaxSlots> g_buf{};
std::atomic<uint64_t> g_nextInvocationId{ 1 };

// A single reserved-but-unresolved ProcessMovement invocation's
// bounded-tier record. Engine-agnostic: only opaque void* identity and
// plain floats/bools, no CS2/HL2SDK type anywhere -- this is what keeps
// the whole positional state machine below exercisable by a standalone
// host-side test binary with no engine dependency.
struct Gate2CFrame
{
    int slot = -1;
    void* services = nullptr;
    void* moveData = nullptr;
    uint64_t invocationId = 0;
    uint32_t generation = 0;
    bool hasData = false; // false: non-recording marker -- every field below is meaningless.
                           // True for admission-closed placeholders too (see barrierHeld).
    // True iff this entry reserved an outstandingFrames barrier slot at
    // ReserveFrame time (i.e. admission was genuinely open). False for an
    // admission-closed placeholder -- pushed purely to preserve this
    // thread's positional ordering (see ReserveFrame's header comment),
    // never counted toward Start()'s barrier, and never carries data
    // (hasData stays false for these, same as any other marker).
    bool barrierHeld = false;
    float preForward = 0, preSide = 0;
    float injectedForward = 0, injectedSide = 0;
    bool wroteIntent = false;
    bool lockAllActive = false;
};

// Overflow-tier entry: same `barrierHeld` distinction as Gate2CFrame,
// without the rest of the payload (this tier never carries data).
struct Gate2COverflowEntry
{
    int slot = -1;
    bool barrierHeld = false;
};

// ---- Per-thread positional LIFO state ------------------------------
// thread_local, mirroring KHook's own g_saved_params (see this file's
// header comment for the source evidence this mirrors). One stack per
// thread, NOT per slot -- one thread can process several slots'
// ProcessMovement calls sequentially or nested, and POST resolution is
// purely positional, so every real PRE that needs tracking at all must
// occupy the correct position in ONE shared per-thread ordering
// regardless of which slot it belongs to.
thread_local std::array<Gate2CFrame, kGate2CMaxFrameDepth> t_frames{};
thread_local int t_depth = 0;

// Overflow tier: same ordering, coarser payload (slot + barrierHeld only).
thread_local std::array<Gate2COverflowEntry, kGate2COverflowCap> t_overflowSlots{};
thread_local int t_overflowDepth = 0;

// Sticky once set (see ReserveFrame): this thread can no longer safely
// track ANY further Gate2C invocation. Never cleared -- there is no safe
// way to know, after the fact, that the backlog which caused this has
// fully drained versus merely been abandoned, so this is a permanent,
// thread-scoped shutdown rather than a resettable condition.
thread_local bool t_fatalDepth = false;

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

bool DrainWriters(SlotState& st)
{
    for (int i = 0; i < kMaxDrainIterations; ++i)
    {
        if (st.inFlight.load(std::memory_order_seq_cst) == 0) return true;
    }
    return st.inFlight.load(std::memory_order_seq_cst) == 0;
}

// Bounded wait for the lifecycle barrier (see SlotState::outstandingFrames
// and Start()'s comment). Same shape as DrainWriters, same "bounded spin,
// not an OS wait" rationale -- deliberately not merged with DrainWriters
// since the two counters track different things (buffer-write-in-progress
// vs. reserved-but-unresolved) and Start() needs both confirmed zero, not
// either one.
bool DrainOutstandingFrames(SlotState& st)
{
    for (int i = 0; i < kMaxDrainIterations; ++i)
    {
        if (st.outstandingFrames.load(std::memory_order_seq_cst) == 0) return true;
    }
    return st.outstandingFrames.load(std::memory_order_seq_cst) == 0;
}

// Sets `poisoned` and forces `active` false. The shared response to
// every pairing-correctness anomaly this file can detect (overflow,
// positional mismatch, fatal depth) -- see each call site's own counter
// for which anomaly actually happened.
void PoisonSlot(SlotState& st)
{
    st.poisoned.store(true, std::memory_order_relaxed);
    st.active.store(false, std::memory_order_release);
}

void MarkOverflow(SlotState& st)
{
    st.overflow.fetch_add(1, std::memory_order_relaxed);
    PoisonSlot(st);
}

void MarkFatalDepth(int slot)
{
    if (!ValidSlot(slot)) return;
    SlotState& st = g_state[slot];
    st.fatalDepth.fetch_add(1, std::memory_order_relaxed);
    PoisonSlot(st);
}

void MarkPositionalMismatch(int slot)
{
    if (!ValidSlot(slot)) return;
    SlotState& st = g_state[slot];
    st.positionalMismatch.fetch_add(1, std::memory_order_relaxed);
    PoisonSlot(st);
}

// Called only from the fatal-depth transition point in ReserveFrame,
// exactly once per thread's lifetime (t_fatalDepth is set immediately
// after this runs and never cleared). Poisons every DISTINCT slot
// currently represented anywhere in either tier -- not just the
// triggering invocation's own slot -- because every one of those
// entries is about to become permanently unresolvable (see
// BeginResolveFrame's fatal-depth path): all of them are equally
// stranded, not just the one whose call happened to trip the threshold.
void PoisonEveryTrackedSlot(int triggeringSlot)
{
    for (int i = 0; i < t_depth; ++i) MarkFatalDepth(t_frames[i].slot);
    for (int i = 0; i < t_overflowDepth; ++i) MarkFatalDepth(t_overflowSlots[i].slot);
    MarkFatalDepth(triggeringSlot);
}

uint64_t NextInvocationIdInternal() { return g_nextInvocationId.fetch_add(1, std::memory_order_relaxed); }

void LogSample(int slot, const char* bucket, const Gate2CSample& s)
{
    BC_LOG_INFO(
        "[gate2c][slot=%d][%s] w=%u gen=%u inv=%llu phase=%s scale=%.2f wroteIntent=%d lockAll=%d "
        "pre=(%.4f,%.4f) injected=(%.4f,%.4f) post=(%.4f,%.4f) "
        "postVel=(%.4f,%.4f,%.4f) postOrigin=(%.4f,%.4f,%.4f)\n",
        slot, bucket, s.windowSeq, s.generation, static_cast<unsigned long long>(s.invocationId), PhaseName(s.phase),
        s.writeScale, s.wroteIntent ? 1 : 0, s.lockAllActive ? 1 : 0, s.preForward, s.preSide, s.injectedForward,
        s.injectedSide, s.postForward, s.postSide, s.postVelX, s.postVelY, s.postVelZ, s.postOriginX, s.postOriginY,
        s.postOriginZ);
}

// Only called after the caller has confirmed (via DrainWriters) that no
// writer can be mid-write. Read-only with respect to the buffers.
void EmitSummary(int slot, bool aborted)
{
    const SlotState& st = g_state[slot];
    const Gate2CBuffers& b = g_buf[slot];

    const uint32_t nA = std::min<uint32_t>(b.activationCount.load(std::memory_order_relaxed), kActivationCap);
    const uint32_t nS = std::min<uint32_t>(b.steadyCount.load(std::memory_order_relaxed), kSteadyCap);
    const uint32_t nC = std::min<uint32_t>(b.cancellationCount.load(std::memory_order_relaxed), kCancelCap);
    const uint32_t nP = std::min<uint32_t>(b.postCancelCount.load(std::memory_order_relaxed), kPostCap);
    // INVALID takes priority over every other label: a poisoning event
    // means pairing correctness could not be guaranteed for the rest of
    // this generation on the affected thread, so counted samples up to
    // that point are not offered as proof the whole window is
    // trustworthy -- see PoisonSlot's call sites. Checked before
    // ABORTED/COMPLETE/PARTIAL.
    const bool invalid = st.poisoned.load(std::memory_order_relaxed);
    const char* result = invalid  ? "INVALID"
                          : aborted ? "ABORTED"
                          : (nA > 0 && nS > 0 && st.finalized.load()) ? "COMPLETE"
                                                                        : "PARTIAL";

    BC_LOG_INFO("[gate2c][slot=%d] ==== FINALIZE RESULT=%s generation=%u ====\n", slot, result,
                st.generation.load(std::memory_order_relaxed));
    if (invalid)
    {
        BC_LOG_INFO(
            "[gate2c][slot=%d] INVALID: pairing correctness could not be guaranteed for the rest of this "
            "generation on at least one thread (bounded-tier overflow, a positional identity mismatch, or "
            "fatal same-thread depth exhaustion -- see overflow/positionalMismatch/fatalDepth below). Do NOT "
            "treat samples captured before the anomaly as proof the rest of the window is valid.\n",
            slot);
    }
    BC_LOG_INFO(
        "[gate2c][slot=%d] captured=%u dropped=%u overflow=%u unmatchedPost=%u staleGeneration=%u "
        "nonRecordingMatch=%u positionalMismatch=%u fatalDepth=%u admissionClosedTracked=%u "
        "outstandingFrames=%d writeScale=%.2f\n",
        slot, st.captured.load(), st.dropped.load(), st.overflow.load(), st.unmatchedPost.load(),
        st.staleGeneration.load(), st.nonRecordingMatch.load(), st.positionalMismatch.load(), st.fatalDepth.load(),
        st.admissionClosedTracked.load(), st.outstandingFrames.load(std::memory_order_seq_cst),
        st.writeScale.load(std::memory_order_relaxed));

    for (uint32_t i = 0; i < nA; ++i) LogSample(slot, "Activation", b.activation[i]);
    for (uint32_t i = 0; i < nS; ++i) LogSample(slot, "SteadyState", b.steady[i]);
    for (uint32_t i = 0; i < nC; ++i) LogSample(slot, "Cancellation", b.cancellation[i]);
    for (uint32_t i = 0; i < nP; ++i) LogSample(slot, "PostCancel", b.postCancel[i]);
    BC_LOG_INFO("[gate2c][slot=%d] ==== END ====\n", slot);
}

// Old RecordResult's body, unchanged logic: performs the one bounded,
// atomically-claimed write for a positionally-resolved, identity-
// verified invocation. Called only from ResolveFrame.
void WriteSample(int slot, uint32_t frameGeneration, uint64_t invocationId, int64_t nowMs, float preForward,
                  float preSide, bool wroteIntent, float injectedForward, float injectedSide, bool lockAllActive,
                  float postForward, float postSide, float velX, float velY, float velZ, float originX,
                  float originY, float originZ)
{
    SlotState& st = g_state[slot];

    WriterGuard guard(st.inFlight);
    if (st.admitting.load(std::memory_order_seq_cst) == 0) return; // window closed; drop silently -- the
                                                                      // invocation's PRE already happened
                                                                      // under an admitted window, but
                                                                      // nothing was ever written to the
                                                                      // shared buffer, so there is nothing
                                                                      // to undo here.
    if (st.poisoned.load(std::memory_order_relaxed))
    {
        // Reachable in normal operation, not just defense in depth: an
        // overflow/mismatch/fatal-depth event on one thread poisons the
        // slot, but bounded-tier entries already reserved on OTHER
        // threads before that moment are deliberately left pending (see
        // MarkOverflow's callers) and will still reach here via their
        // own genuine positional resolution. Their outstandingFrames
        // accounting is correct and unaffected by poisoning, which only
        // refuses writing a sample: the generation as a whole is
        // reported INVALID once poisoned, so this match's data is not
        // offered as trustworthy.
        st.dropped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (frameGeneration != st.generation.load(std::memory_order_relaxed))
    {
        st.staleGeneration.fetch_add(1, std::memory_order_relaxed);
        return; // this frame belongs to an earlier (or, in principle, a
                // not-yet-possible later) generation than the one
                // currently admitting -- never recorded into the wrong
                // test's buffers.
    }

    uint32_t w = st.windowCalls.fetch_add(1, std::memory_order_relaxed) + 1;
    bool cancelled = st.cancelled.load(std::memory_order_acquire);
    Phase phase;
    if (!cancelled)
    {
        phase = (w <= kActivationWindowCalls) ? Phase::Activation : Phase::SteadyState;
    }
    else
    {
        uint32_t mark = st.callsAtCancelMark.load(std::memory_order_relaxed);
        uint32_t sinceCancel = (w > mark) ? (w - mark) : 0;
        phase = (sinceCancel <= kCancellationWindowCalls) ? Phase::Cancellation : Phase::PostCancel;
    }

    Gate2CSample s{};
    s.windowSeq = w;
    s.phase = phase;
    s.invocationId = invocationId;
    s.generation = frameGeneration;
    s.writeScale = st.writeScale.load(std::memory_order_relaxed);
    s.wroteIntent = wroteIntent;
    s.preForward = preForward;
    s.preSide = preSide;
    s.injectedForward = injectedForward;
    s.injectedSide = injectedSide;
    s.lockAllActive = lockAllActive;
    s.postForward = postForward;
    s.postSide = postSide;
    s.postVelX = velX;
    s.postVelY = velY;
    s.postVelZ = velZ;
    s.postOriginX = originX;
    s.postOriginY = originY;
    s.postOriginZ = originZ;

    Gate2CBuffers& buf = g_buf[slot];
    uint32_t idx = 0;
    bool claimed = false;
    switch (phase)
    {
    case Phase::Activation:
        claimed = ClaimSlot(buf.activationCount, kActivationCap, idx);
        if (claimed) buf.activation[idx] = s;
        break;
    case Phase::SteadyState:
    {
        int64_t prev = st.steadyLastSampleMs.load(std::memory_order_relaxed);
        if (nowMs - prev >= kSteadyStateMinIntervalMs &&
            st.steadyLastSampleMs.compare_exchange_strong(prev, nowMs, std::memory_order_relaxed))
        {
            claimed = ClaimSlot(buf.steadyCount, kSteadyCap, idx);
            if (claimed) buf.steady[idx] = s;
        }
        else
        {
            st.dropped.fetch_add(1, std::memory_order_relaxed); // deliberately time-decimated, not a
            return;                                              // full-buffer drop; don't double count
        }
        break;
    }
    case Phase::Cancellation:
        claimed = ClaimSlot(buf.cancellationCount, kCancelCap, idx);
        if (claimed) buf.cancellation[idx] = s;
        break;
    case Phase::PostCancel:
        claimed = ClaimSlot(buf.postCancelCount, kPostCap, idx);
        if (claimed) buf.postCancel[idx] = s;
        break;
    default:
        break;
    }
    if (claimed) st.captured.fetch_add(1, std::memory_order_relaxed);
    else st.dropped.fetch_add(1, std::memory_order_relaxed);
}

} // namespace

int Start(int slot, float writeScale)
{
    if (!ValidSlot(slot)) return kStatusInvalidSlot;
    SlotState& st = g_state[slot];

    st.active.store(false, std::memory_order_release); // stop new top-level activity first

    // Close admission BEFORE waiting on the lifecycle barrier. `admitting`
    // is the SAME gate ReserveFrame checks (after it has already
    // reserved) and WriteSample checks (after its own WriterGuard
    // registration). Closing it here, before the barrier wait below, is
    // what closes the race between Start() observing outstandingFrames==0
    // and a concurrent PRE reserving a frame: seq_cst gives every access
    // to `admitting` and to `outstandingFrames` a single total order, so
    // any ReserveFrame call is either (a) fully ordered before this
    // store -- its increment is then visible to and waited for by
    // DrainOutstandingFrames below -- or (b) its own admitting-check is
    // ordered after this store, reads 0, and it reverts its own
    // increment before returning AdmissionClosed to the caller. There is
    // no interleaving in which a frame is both reserved and invisible to
    // the wait below.
    st.admitting.store(0, std::memory_order_seq_cst);

    // Lifecycle barrier: do not proceed to a new generation while a
    // reserved-but-unresolved Gate2C invocation could still exist on ANY
    // thread for this slot -- see this function's own header comment and
    // SlotState::outstandingFrames. Because outstandingFrames is only
    // ever decremented when ResolveFrame positionally resolves the exact
    // reservation that incremented it (never on discard/refusal/fatal
    // depth), this wait genuinely proves no real invocation's POST is
    // still outstanding once it succeeds. Fails closed: on timeout,
    // `active` stays false and this returns BUSY without touching
    // generation/buffers, and admission stays closed (a subsequent
    // Start() call will simply re-attempt the same wait).
    if (!DrainOutstandingFrames(st)) return kStatusBusy;

    if (!DrainWriters(st)) return kStatusBusy;

    st.finalized.store(false, std::memory_order_relaxed);
    st.abortedFlag.store(false, std::memory_order_relaxed);
    st.cancelled.store(false, std::memory_order_relaxed);
    st.windowCalls.store(0, std::memory_order_relaxed);
    st.callsAtCancelMark.store(0, std::memory_order_relaxed);
    st.steadyLastSampleMs.store(0, std::memory_order_relaxed);
    st.captured.store(0, std::memory_order_relaxed);
    st.dropped.store(0, std::memory_order_relaxed);
    st.overflow.store(0, std::memory_order_relaxed);
    st.unmatchedPost.store(0, std::memory_order_relaxed);
    st.staleGeneration.store(0, std::memory_order_relaxed);
    st.nonRecordingMatch.store(0, std::memory_order_relaxed);
    st.positionalMismatch.store(0, std::memory_order_relaxed);
    st.fatalDepth.store(0, std::memory_order_relaxed);
    st.admissionClosedTracked.store(0, std::memory_order_relaxed);
    st.poisoned.store(false, std::memory_order_relaxed);
    st.writeScale.store(writeScale, std::memory_order_relaxed);
    g_buf[slot].Reset();

    // Advance generation BEFORE reopening admission: any frame still
    // reserved under the previous generation (e.g. left in some thread's
    // stack because its POST never arrived) will now fail the generation
    // check in WriteSample even if that POST arrives after this point.
    st.generation.fetch_add(1, std::memory_order_relaxed);

    st.admitting.store(1, std::memory_order_seq_cst);
    st.active.store(true, std::memory_order_release); // enable last, only once state/buffers are ready
    return kStatusOk;
}

void MarkCancelled(int slot)
{
    if (!ValidSlot(slot)) return;
    SlotState& st = g_state[slot];
    if (st.admitting.load(std::memory_order_seq_cst) == 0) return; // no active window
    if (st.cancelled.exchange(true, std::memory_order_acq_rel)) return; // already marked; idempotent
    st.callsAtCancelMark.store(st.windowCalls.load(std::memory_order_relaxed), std::memory_order_relaxed);
}

int Finalize(int slot, int aborted)
{
    if (!ValidSlot(slot)) return kStatusInvalidSlot;
    SlotState& st = g_state[slot];

    st.active.store(false, std::memory_order_release); // stop new pushes/writes first
    st.admitting.store(0, std::memory_order_seq_cst);  // close admission for in-flight writers
    if (!DrainWriters(st))
    {
        st.active.store(true, std::memory_order_release); // could not confirm quiescence -- re-open
                                                             // and report busy rather than read/reset
                                                             // a possibly still-active buffer
        return kStatusBusy;
    }

    if (st.finalized.exchange(true, std::memory_order_acq_rel))
    {
        EmitSummary(slot, st.abortedFlag.load(std::memory_order_acquire));
        return kStatusRepeat;
    }
    st.abortedFlag.store(aborted != 0, std::memory_order_release);
    EmitSummary(slot, aborted != 0);
    return kStatusOk;
}

bool IsActiveForSlot(int slot)
{
    if (!ValidSlot(slot)) return false;
    return g_state[slot].active.load(std::memory_order_acquire);
}

float WriteScaleForSlot(int slot)
{
    if (!ValidSlot(slot)) return 0.0F;
    return g_state[slot].writeScale.load(std::memory_order_relaxed);
}

Gate2CReservation ReserveFrame(int slot, void* services, void* moveData)
{
    Gate2CReservation result;
    if (!ValidSlot(slot)) return result; // NotTracked

    if (t_fatalDepth)
    {
        // This thread can no longer safely track anything -- see this
        // function's own header comment. Decline unconditionally.
        MarkFatalDepth(slot);
        return result; // NotTracked
    }

    const bool active = IsActiveForSlot(slot);

    // Cheap common-case fast path: Gate2C is inactive for this call AND
    // this thread has nothing else pending (any slot). Nothing to
    // reserve, nothing to track.
    if (!active && t_depth == 0 && t_overflowDepth == 0)
    {
        return result; // NotTracked
    }

    SlotState& st = g_state[slot];

    // Question 1 (see this function's header comment): does this
    // invocation get a BARRIER reservation? Register BEFORE checking
    // admission -- this ordering (not the check alone) is what closes
    // the race against a concurrent Start(): see Start()'s own comment
    // for the full proof. A closed admission reverts the increment, but
    // -- unlike the earlier, unsound version of this function -- does
    // NOT skip question 2 below: this invocation still needs a POSITION.
    st.outstandingFrames.fetch_add(1, std::memory_order_seq_cst);
    bool barrierHeld = true;
    if (st.admitting.load(std::memory_order_seq_cst) == 0)
    {
        st.outstandingFrames.fetch_sub(1, std::memory_order_seq_cst);
        barrierHeld = false;
    }

    // Question 2: does this invocation get a POSITION? Always, as long
    // as tier capacity remains -- independent of barrierHeld. An
    // admission-closed invocation (barrierHeld=false) is pushed exactly
    // like an inactive marker: dataless, not counted toward the barrier,
    // but occupying its own position so its own eventual POST cannot
    // positionally consume an older, still-pending invocation's entry.
    if (t_overflowDepth == 0 && t_depth < kGate2CMaxFrameDepth)
    {
        // Bounded data/marker tier has room.
        Gate2CFrame& frame = t_frames[t_depth];
        frame = Gate2CFrame{};
        frame.slot = slot;
        frame.services = services;
        frame.moveData = moveData;
        frame.barrierHeld = barrierHeld;
        if (barrierHeld)
        {
            frame.invocationId = NextInvocationIdInternal();
            frame.generation = st.generation.load(std::memory_order_relaxed);
        }
        ++t_depth;

        if (barrierHeld)
        {
            result.admission = Gate2CAdmission::Reserved;
            result.active = active;
            result.invocationId = frame.invocationId;
            result.generation = frame.generation;
        }
        else
        {
            st.admissionClosedTracked.fetch_add(1, std::memory_order_relaxed);
            result.admission = Gate2CAdmission::AdmissionClosed;
        }
        return result;
    }

    if (t_overflowDepth < kGate2COverflowCap)
    {
        // Bounded tier is full (or already in the overflow regime, which
        // is sticky by construction: t_depth stays pinned at capacity
        // for as long as t_overflowDepth > 0, since nothing pops the
        // bounded tier until the overflow tier has fully drained -- see
        // BeginResolveFrame). This invocation gets a position, but no
        // payload -- caller must not write CMoveData or call CommitFrame
        // (true for both Overflow and AdmissionClosed admission values).
        t_overflowSlots[t_overflowDepth] = { slot, barrierHeld };
        ++t_overflowDepth;
        if (barrierHeld)
        {
            // A genuine capacity anomaly -- an invocation that WOULD have
            // been trackable (admission open) lost its data slot to
            // depth pressure. Worth poisoning/counting as INVALID.
            MarkOverflow(st);
            result.admission = Gate2CAdmission::Overflow;
        }
        else
        {
            // Not a capacity anomaly by itself -- admission was already
            // closed for an unrelated reason (an ordinary Finalize/Start
            // boundary); this entry exists purely to preserve position,
            // and landing in the overflow tier instead of the bounded
            // one is incidental. Does not poison.
            st.admissionClosedTracked.fetch_add(1, std::memory_order_relaxed);
            result.admission = Gate2CAdmission::AdmissionClosed;
        }
        return result;
    }

    // Both tiers exhausted on this thread simultaneously -- see this
    // function's header comment. Decline this invocation entirely: undo
    // its barrier reservation if it held one, poison every slot
    // currently represented in either tier (not just this one), then go
    // sticky-fatal for this thread so no further push/pop is attempted
    // here.
    if (barrierHeld) st.outstandingFrames.fetch_sub(1, std::memory_order_seq_cst);
    PoisonEveryTrackedSlot(slot);
    t_fatalDepth = true;
    result.admission = Gate2CAdmission::NotTracked;
    return result;
}

void CommitFrame(int slot, uint64_t invocationId, bool hasData, float preForward, float preSide, bool wroteIntent,
                  float injectedForward, float injectedSide, bool lockAllActive)
{
    if (t_depth == 0) return; // contract violation (Commit without a preceding Reserved); unreachable
                               // if InputInjector.cpp's PRE hook honors ReserveFrame's contract.
    Gate2CFrame& frame = t_frames[t_depth - 1];
    if (frame.invocationId != invocationId || frame.slot != slot)
    {
        // The caller violated the "commit immediately after reserve,
        // exactly once, synchronously" contract this thread's positional
        // correctness depends on. Do not write into a frame that may not
        // be the one the caller believes it is; poison the slot the
        // caller believes this is for and leave the frame's hasData at
        // its reset default (false) so nothing fabricated is ever
        // recorded from it.
        if (ValidSlot(slot)) PoisonSlot(g_state[slot]);
        return;
    }

    frame.hasData = hasData;
    if (hasData)
    {
        frame.preForward = preForward;
        frame.preSide = preSide;
        frame.wroteIntent = wroteIntent;
        frame.injectedForward = injectedForward;
        frame.injectedSide = injectedSide;
        frame.lockAllActive = lockAllActive;
    }
}

Gate2CResolution BeginResolveFrame(int slot, void* services, void* moveData)
{
    Gate2CResolution result;

    if (t_fatalDepth)
    {
        // Nothing is touched once fatal -- see this file's header
        // comment and ReserveFrame's for why this is still fail-closed.
        MarkFatalDepth(slot);
        result.outcome = Gate2CResolutionOutcome::FatalDepth;
        return result;
    }

    if (t_overflowDepth > 0)
    {
        --t_overflowDepth;
        const Gate2COverflowEntry entry = t_overflowSlots[t_overflowDepth];
        if (entry.slot == slot)
        {
            if (entry.barrierHeld && ValidSlot(slot))
                g_state[slot].outstandingFrames.fetch_sub(1, std::memory_order_seq_cst);
        }
        else
        {
            // Positional mismatch even under the overflow tier's coarse
            // (slot-only) check. Release the reservation (if any) against
            // the slot that actually made it (entry.slot) -- that is the
            // reservation genuinely being resolved -- and poison every
            // slot involved; never trust or silently ignore this.
            if (entry.barrierHeld && ValidSlot(entry.slot))
                g_state[entry.slot].outstandingFrames.fetch_sub(1, std::memory_order_seq_cst);
            if (ValidSlot(entry.slot)) MarkPositionalMismatch(entry.slot);
            if (ValidSlot(slot) && slot != entry.slot) MarkPositionalMismatch(slot);
        }
        // An AdmissionClosed placeholder that landed in the overflow tier
        // resolves the same way as a genuine overflow entry from the
        // caller's perspective (no data either way) -- the distinction
        // (barrierHeld) only affects barrier accounting above, not the
        // outcome reported here.
        result.outcome = Gate2CResolutionOutcome::OverflowResolved;
        return result; // overflow-tier entries never carry data to record
    }

    if (t_depth == 0)
    {
        // Nothing was ever reserved for this call on this thread.
        if (ValidSlot(slot)) g_state[slot].unmatchedPost.fetch_add(1, std::memory_order_relaxed);
        result.outcome = Gate2CResolutionOutcome::Unmatched;
        return result;
    }

    --t_depth;
    const Gate2CFrame frame = t_frames[t_depth]; // copy out before any further mutation

    // Release the reservation -- if this entry held one (see
    // ReserveFrame: an AdmissionClosed placeholder never did) -- against
    // the slot that actually made it (frame.slot), NOT necessarily this
    // call's own `slot` parameter -- the two are only guaranteed equal
    // when the positional invariant held, which is exactly what the
    // assertion below checks.
    if (frame.barrierHeld && ValidSlot(frame.slot))
        g_state[frame.slot].outstandingFrames.fetch_sub(1, std::memory_order_seq_cst);

    const bool identityOk = (frame.slot == slot && frame.services == services && frame.moveData == moveData);
    if (!identityOk)
    {
        if (ValidSlot(frame.slot)) MarkPositionalMismatch(frame.slot);
        if (ValidSlot(slot) && slot != frame.slot) MarkPositionalMismatch(slot);
        result.outcome = Gate2CResolutionOutcome::Mismatch;
        return result; // never trust or record data from a mismatched pairing
    }

    if (!frame.hasData)
    {
        // Non-recording marker: this invocation's PRE ran while Gate2C
        // was inactive, OR while admission was closed (barrierHeld was
        // false -- see ReserveFrame). Correctly (positionally) resolved
        // -- proving no mismatch occurred -- but there is no real
        // PRE/INJECTED data either way.
        if (ValidSlot(slot)) g_state[slot].nonRecordingMatch.fetch_add(1, std::memory_order_relaxed);
        result.outcome = Gate2CResolutionOutcome::NonRecording;
        return result;
    }

    result.outcome = Gate2CResolutionOutcome::DataReady;
    result.generation = frame.generation;
    result.invocationId = frame.invocationId;
    result.preForward = frame.preForward;
    result.preSide = frame.preSide;
    result.wroteIntent = frame.wroteIntent;
    result.injectedForward = frame.injectedForward;
    result.injectedSide = frame.injectedSide;
    result.lockAllActive = frame.lockAllActive;
    return result;
}

void CommitResolvedSample(int slot, const Gate2CResolution& resolution, int64_t nowMs, float postForward,
                           float postSide, float velX, float velY, float velZ, float originX, float originY,
                           float originZ)
{
    if (!ValidSlot(slot) || resolution.outcome != Gate2CResolutionOutcome::DataReady) return;
    WriteSample(slot, resolution.generation, resolution.invocationId, nowMs, resolution.preForward,
                resolution.preSide, resolution.wroteIntent, resolution.injectedForward, resolution.injectedSide,
                resolution.lockAllActive, postForward, postSide, velX, velY, velZ, originX, originY, originZ);
}

} // namespace cs2bc::gate2c

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

// One complete, IDENTITY-MATCHED PRE+INJECTED+POST observation for a
// single ProcessMovement invocation. Written exactly once (from
// RecordResult) after the POST hook has found and verified the frame
// that corresponds to this exact call -- see InputInjector.cpp's
// Gate2CFrame stack for how that matching happens.
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
    // call; this decides whether a new frame is pushed / CMoveData is
    // written at all. The POST hook does NOT read this -- see
    // `generation` below for why it doesn't need to.
    std::atomic<bool> active{ false };
    std::atomic<float> writeScale{ 0.0F };

    // Advanced once per successful Start. A frame pushed under
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

    // Pairing-failure counters, each reported distinctly in the summary
    // rather than folded into a generic "dropped" bucket.
    std::atomic<uint32_t> overflow{ 0 };
    std::atomic<uint32_t> unmatchedPost{ 0 };
    std::atomic<uint32_t> orphanedFrames{ 0 };
    std::atomic<uint32_t> staleGeneration{ 0 };
    std::atomic<uint32_t> nonRecordingMatch{ 0 }; // correct pairing, no data -- see header comment

    // Set by RecordOverflowPoisoned; once true, EmitSummary reports this
    // generation's result as INVALID regardless of captured/dropped counts
    // -- a poisoning event means pairing correctness could no longer be
    // guaranteed on the affected thread from that point on, so whatever
    // was captured before it is not treated as proof the rest is trustworthy.
    std::atomic<bool> poisoned{ false };
};

std::array<SlotState, kMaxSlots> g_state{};
std::array<Gate2CBuffers, kMaxSlots> g_buf{};
std::atomic<uint64_t> g_nextInvocationId{ 1 };

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
    // INVALID takes priority over every other label: a poisoning event means
    // pairing correctness could not be guaranteed for the rest of this
    // generation on the affected thread, so counted samples up to that point
    // are not offered as proof the whole window is trustworthy -- see
    // RecordOverflowPoisoned. Checked before ABORTED/COMPLETE/PARTIAL.
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
            "[gate2c][slot=%d] INVALID: at least one thread was poisoned by depth overflow during this "
            "generation -- pairing correctness could not be guaranteed from that point on. Do NOT treat "
            "samples captured before the overflow as proof the rest of the window is valid.\n",
            slot);
    }
    BC_LOG_INFO(
        "[gate2c][slot=%d] captured=%u dropped=%u overflow=%u unmatchedPost=%u orphanedFrames=%u "
        "staleGeneration=%u nonRecordingMatch=%u writeScale=%.2f\n",
        slot, st.captured.load(), st.dropped.load(), st.overflow.load(), st.unmatchedPost.load(),
        st.orphanedFrames.load(), st.staleGeneration.load(), st.nonRecordingMatch.load(),
        st.writeScale.load(std::memory_order_relaxed));

    for (uint32_t i = 0; i < nA; ++i) LogSample(slot, "Activation", b.activation[i]);
    for (uint32_t i = 0; i < nS; ++i) LogSample(slot, "SteadyState", b.steady[i]);
    for (uint32_t i = 0; i < nC; ++i) LogSample(slot, "Cancellation", b.cancellation[i]);
    for (uint32_t i = 0; i < nP; ++i) LogSample(slot, "PostCancel", b.postCancel[i]);
    BC_LOG_INFO("[gate2c][slot=%d] ==== END ====\n", slot);
}

} // namespace

int Start(int slot, float writeScale)
{
    if (!ValidSlot(slot)) return kStatusInvalidSlot;
    SlotState& st = g_state[slot];

    st.active.store(false, std::memory_order_release); // stop new pushes/writes before resetting anything
    st.admitting.store(0, std::memory_order_seq_cst);
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
    st.orphanedFrames.store(0, std::memory_order_relaxed);
    st.staleGeneration.store(0, std::memory_order_relaxed);
    st.nonRecordingMatch.store(0, std::memory_order_relaxed);
    st.poisoned.store(false, std::memory_order_relaxed);
    st.writeScale.store(writeScale, std::memory_order_relaxed);
    g_buf[slot].Reset();

    // Advance generation BEFORE reopening admission: any frame still
    // pushed under the previous generation (e.g. left in some thread's
    // stack because its POST never arrived) will now fail the generation
    // check in RecordResult even if that POST arrives after this point.
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

uint32_t CurrentGeneration(int slot)
{
    if (!ValidSlot(slot)) return 0;
    return g_state[slot].generation.load(std::memory_order_relaxed);
}

uint64_t NextInvocationId() { return g_nextInvocationId.fetch_add(1, std::memory_order_relaxed); }

void RecordOverflowPoisoned(int slot)
{
    if (!ValidSlot(slot)) return;
    SlotState& st = g_state[slot];
    st.overflow.fetch_add(1, std::memory_order_relaxed);
    st.poisoned.store(true, std::memory_order_relaxed); // sticky for this generation; see Start()'s reset
}

void RecordUnmatchedPost(int slot)
{
    if (!ValidSlot(slot)) return;
    g_state[slot].unmatchedPost.fetch_add(1, std::memory_order_relaxed);
}

void RecordOrphanedFrame(int slot)
{
    if (!ValidSlot(slot)) return;
    g_state[slot].orphanedFrames.fetch_add(1, std::memory_order_relaxed);
}

void RecordNonRecordingMatch(int slot)
{
    if (!ValidSlot(slot)) return;
    g_state[slot].nonRecordingMatch.fetch_add(1, std::memory_order_relaxed);
}

void RecordResult(int slot, uint32_t frameGeneration, uint64_t invocationId, int64_t nowMs, float preForward,
                   float preSide, bool wroteIntent, float injectedForward, float injectedSide, bool lockAllActive,
                   float postForward, float postSide, float velX, float velY, float velZ, float originX,
                   float originY, float originZ)
{
    if (!ValidSlot(slot)) return;
    SlotState& st = g_state[slot];

    WriterGuard guard(st.inFlight);
    if (st.admitting.load(std::memory_order_seq_cst) == 0) return; // window closed; drop silently -- the
                                                                      // frame's PRE already happened under
                                                                      // an admitted window, but nothing was
                                                                      // ever written to the shared buffer,
                                                                      // so there is nothing to undo here.
    if (st.poisoned.load(std::memory_order_relaxed))
    {
        // Defense in depth: InputInjector.cpp's PRE hook already stops
        // pushing frames once a thread is poisoned, so this should be
        // unreachable in practice -- but refuse here too rather than rely
        // solely on that invariant holding.
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

} // namespace cs2bc::gate2c

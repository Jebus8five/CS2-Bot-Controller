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
// PRE/POST pairing model (redesigned after a source investigation of the
// exact vendored KHook commit this project's CI resolves --
// Kenzzer/KHook @ 40d233d160b5bf60cc3e732939142b222fbd8ece, reached via
// metamod-source @ fa6f80e4662e5b96cc2e97722d812f374581dfd8's pinned
// submodule -- see src/detour.cpp in that commit for everything cited
// below):
//
//   - KHook's own per-call state (`g_saved_params`, a thread_local
//     std::stack<AsmLoopDetails*>) is pushed exactly once per real,
//     non-recall entry into a hooked function, and popped exactly once
//     when that call's post-loop finishes -- BeginDetour/EndDetour in
//     detour.cpp. This is a genuine, unconditional, per-invocation LIFO
//     stack, not a convention: a nested/reentrant call to the SAME
//     hooked function on the SAME thread pushes its own entry and must
//     fully resolve (pre-loop, maybe-original, post-loop, pop) before
//     the outer call's remaining instructions can resume, because that
//     is ordinary recursive call-stack unwinding, not a KHook policy
//     choice.
//   - PRE and POST for one real invocation cannot be on different
//     threads: the detour is a JIT-generated trampoline that fully
//     replaces the call site, and pre-loop -> maybe-original ->
//     post-loop are sequential instructions inside ONE synchronous call.
//     There is no dispatch queue or thread handoff.
//   - The post-loop is UNCONDITIONAL: detour.cpp's x64 JIT sequence has
//     no branch that skips it when the original was superseded/skipped
//     (`CallOriginalState::CompleteSkipped` is an explicitly valid
//     terminal state EndDetour accepts, not an error). POST fires for
//     every PRE KHook delivered to a consumer, regardless of Supersede.
//   - Multiple consumers hooking the same target address (another
//     plugin also hooking ProcessMovement) share ONE pre-loop/original/
//     post-loop pass per real invocation; our own consumer still gets
//     exactly one PRE and one POST call per real invocation.
//   - KHook exposes NO invocation-specific token to consumer callbacks.
//     Internally `AsmLoopDetails*` (or equivalently `g_saved_params`'s
//     depth) is a genuine unique-per-invocation identity, but no
//     KHOOK_API function returns it; `GetContextPtr()` returns the
//     constant `context` pointer supplied once at registration, not a
//     per-call id.
//
// Consequently: KHook itself never "declines" to track an invocation --
// its own stack is unconditional and effectively unbounded. Three
// earlier designs in this file's history tried to reconstruct pairing by
// SEARCHING a bounded, identity-keyed thread_local stack in
// InputInjector.cpp, or by giving POSITIONAL representation to most but
// not all declined invocations; each was shown, by review, to be unsound
// whenever our own bookkeeping could decline to represent a real
// invocation -- a bounded-depth overflow, OR (the case a later review
// specifically identified) an admission-closed refusal -- while KHook's
// own POST for that invocation still fired unconditionally. Critically,
// an admission-closed refusal is NOT a rare capacity anomaly: it happens
// on every ordinary Finalize()/Start() boundary, so this is not a
// theoretical gap -- and pointer identity cannot reliably catch the
// consequence, because pointer reuse across distinct invocations is
// expected, not hypothetical, on this codebase's own evidence. When an
// unrepresented invocation's POST positionally "steals" an older,
// genuinely pending frame's slot, the effect can CASCADE: every
// invocation pushed afterward ends up positionally off-by-one relative
// to what it should be, and once the queue has drained past the
// original gap, a later invocation's real PRE/INJECTED data can end up
// silently paired with a completely unrelated POST-side CMoveData
// reading and recorded as if it were a legitimate sample -- the
// generation/admitting checks in WriteSample do not catch this, because
// by the time it happens the mismatched pairing has "settled" into a
// self-consistent (but wrong) state under the current generation.
//
// The corrected model mirrors KHook's own discipline instead of trying
// to out-guess it, and treats "no barrier reservation" and "no
// positional representation" as two INDEPENDENT questions rather than
// one: this file (not InputInjector.cpp) now owns a thread_local,
// per-thread (not per-slot -- one thread can process several slots'
// calls sequentially or nested) POSITIONAL LIFO stack, and EVERY real
// PRE that needs tracking at all (see ReserveFrame) gets SOME positional
// representation -- a bounded data/marker slot, or, once that's
// exhausted, a same-thread overflow-tier slot -- regardless of whether
// its own barrier reservation succeeded. An admission-closed invocation
// still occupies a position (as a dataless, non-barrier-participating
// entry, exactly like an inactive marker) precisely so that its
// eventual POST resolves ITS OWN position, never an older, still-
// genuinely-pending invocation's. POST always resolves (pops) whatever
// is currently on top, by construction, the same way KHook's own
// g_saved_params does -- never a scan. (slot, services, moveData)
// identity is still recorded for bounded-tier entries and checked at
// resolve time, but ONLY as a secondary diagnostic assertion that the
// positional invariant actually held -- never as the primary matching
// mechanism, and never the thing this design relies on to prevent
// cross-invocation contamination in the first place. A mismatch there is
// treated as a fail-closed signal that the one remaining structural
// assumption (same-thread PRE/POST pairing) did not hold in practice,
// not something to silently paper over.
//
// Lifecycle, owned by the caller (three new exports mirroring Gate2B's):
//   Start(slot, writeScale) -> ... -> MarkCancelled(slot) -> ...
//   -> Finalize(slot, aborted)
//
// Disabled by default: IsActiveForSlot(slot) is false until Start has
// been called for that slot, and false again after Finalize -- this is
// the switch InputInjector.cpp's PRE hook reads (via ReserveFrame) on
// every ProcessMovement invocation. The POST hook (ResolveFrame) does
// NOT gate on this flag -- it must still be able to positionally resolve
// a frame that was reserved just before Finalize ran, so this thread's
// stack is never left unbalanced.
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
// Sequence, in order -- the ORDER is load-bearing, not incidental:
//   1. active := false (stop new top-level Gate2C activity).
//   2. admitting := 0 (seq_cst). This is the SAME gate ReserveFrame
//      checks (after it has already reserved) and the final sample write
//      checks (after its own WriterGuard registration) -- reusing one
//      gate for both closes the race between Start() observing zero and
//      a concurrent PRE registering a frame: every reservation
//      happens-before its own admission check, so seq_cst's single total
//      order guarantees that ANY reservation concurrent with this store
//      either (a) is ordered before it, and is then correctly waited for
//      below, or (b) is ordered after it, reads admitting=0, and reverts
//      itself -- there is no third outcome.
//   3. Bounded wait for outstandingFrames == 0 (DrainOutstandingFrames).
//      Because outstandingFrames is decremented ONLY when ResolveFrame
//      positionally resolves a reservation (bounded-tier pop or overflow
//      counter decrement) and NEVER on a discard/evict/refusal, this
//      wait genuinely means "no reservation anywhere is still waiting
//      for its real POST" once it succeeds -- not merely "we stopped
//      looking." This is what makes step 5's generation bump actually
//      safe: a frame's stamped generation can only mean "no newer
//      generation had begun yet" if a newer generation truly cannot
//      begin while that frame remains genuinely unresolved.
//   4. Bounded wait for inFlight == 0 (DrainWriters, unchanged from
//      Gate2B's own proven protocol) -- no sample write is mid-write.
//   5. Reset counters/buffers, advance generation, reopen admitting and
//      active.
// Fails closed at every wait: if either bounded drain does not confirm
// zero in time, returns kStatusBusy immediately and does not proceed
// past that point -- `active` stays false, the generation is not
// advanced, admission stays closed, and Gate2C remains disabled rather
// than assuming old invocations have completed. A genuinely missing
// POST (a real invocation whose callback never arrives) leaves this
// permanently BUSY for the affected slot until process restart -- this
// is the one residual, honestly-disclosed availability cost of failing
// closed; it is not a correctness gap.
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
// a frame reserved just before this call is still handled safely by
// ResolveFrame if its POST arrives afterward (see CurrentGeneration's
// old role, now internal: the stamped generation simply stops matching
// once a later Start() advances it).
int Finalize(int slot, int aborted);

// True only between a slot's own successful Start and its Finalize. Read
// by ReserveFrame, fresh, on every ProcessMovement invocation, to decide
// whether this invocation gets real diagnostic data. Toggling it never
// requires a rebuild or restart.
bool IsActiveForSlot(int slot);

// The explicit write-scale currently in effect for slot (0 if the slot is
// not active). This is the SAME value the caller passed to Start -- there
// is no second, silently-different scale computed anywhere else.
float WriteScaleForSlot(int slot);

// ---- Frame lifecycle: positional LIFO pairing ----------------------
// Called only from InputInjector.cpp's ProcessMovement PRE/POST hooks,
// exactly once each per real invocation, in this order:
//   PRE:  ReserveFrame(...) -> [if Reserved] exactly one CommitFrame(...)
//   POST: ResolveFrame(...)
// ReserveFrame/CommitFrame/ResolveFrame own ALL of this thread's Gate2C
// position bookkeeping -- InputInjector.cpp holds no Gate2C frame state
// of its own anymore. This keeps the safety-critical state machine free
// of any CS2/HL2SDK dependency (it only ever sees opaque void* pointers
// and plain floats/bools), which is what makes it possible to exercise
// with a standalone host-side test binary -- see
// tests/gate2c_lifecycle_test.cpp.

enum class Gate2CAdmission : std::uint8_t
{
    // Gate2C is inactive for `slot` AND this thread has nothing else
    // pending (no bounded-tier depth, no overflow-tier depth). The
    // overwhelmingly common case once Gate2C has never been touched on
    // this thread: a single cheap check, no reservation made, no barrier
    // touched. The caller does nothing further for this invocation.
    NotTracked,
    // A Start()/Finalize() was concurrently closing admission at the
    // exact moment this call tried to reserve. The barrier reservation
    // attempt was made and reverted internally (see ReserveFrame's
    // comment for the race proof) -- but a POSITION was still reserved
    // for this invocation (a dataless, non-barrier-participating entry,
    // in whichever tier has room), exactly so its own eventual POST
    // cannot positionally consume an older, still-pending invocation's
    // entry. This is invisible to the caller: the caller still does
    // nothing further for this invocation (no CommitFrame call, no
    // CMoveData write) -- the position is fully self-contained at
    // reservation time.
    AdmissionClosed,
    // The bounded data/marker tier for this thread was already
    // exhausted, so this invocation was recorded in a second, smaller
    // same-thread overflow tier that stores only `slot` (no
    // services/moveData, no diagnostic payload) instead of a full
    // data/marker slot. The barrier was still reserved (this invocation
    // IS accounted for; Start() will wait for it), the diagnostic was
    // marked INVALID, and `slot`'s `active` was forced false for the
    // whole slot (cross-thread) -- see ReserveFrame's comment. The
    // caller MUST NOT write CMoveData, call PeekUsercmdMovement, or call
    // CommitFrame for this invocation.
    Overflow,
    // A genuine bounded-tier slot was reserved. The caller MUST call
    // CommitFrame exactly once for this invocation (using `active` to
    // decide whether it captures real PRE/INJECTED data or commits an
    // inert, non-recording marker) before returning from the PRE hook.
    Reserved
};

struct Gate2CReservation
{
    Gate2CAdmission admission = Gate2CAdmission::NotTracked;
    // Gate2C's admitted state for `slot` at the moment of reservation.
    // Only meaningful when admission == Reserved. Because a successful
    // reservation blocks Start() from advancing the generation until
    // this invocation resolves (see Start()'s comment), `active` and
    // `generation` below remain valid for this invocation's ENTIRE
    // lifetime, not just at this instant -- there is no TOCTOU window to
    // worry about between reservation and the later CommitFrame call.
    bool active = false;
    uint64_t invocationId = 0; // only meaningful when admission == Reserved
    uint32_t generation = 0;   // only meaningful when admission == Reserved && active
};

// Called from the PRE hook for EVERY real ProcessMovement invocation
// with non-null moveData -- unconditionally, not behind any caller-side
// gate (the cheap NotTracked fast path lives inside this function, not
// at the call site, so there is exactly one place this decision is
// made). Internally, this function answers TWO INDEPENDENT questions for
// every invocation that needs tracking at all (see the cheap fast path
// below for what "needs tracking" means):
//   1. Does this invocation get a barrier reservation (outstandingFrames
//      incremented, Start() will wait for it)? Only if `admitting` is
//      open at the moment of the register-before-check step below
//      (increment first, then check admitting; revert if it reads
//      closed) -- this ordering is what closes the race against a
//      concurrent Start(), exactly as documented on Start() itself.
//   2. Does this invocation get a POSITION on this thread's LIFO stack
//      (a bounded data/marker slot, or, once that's exhausted, a
//      same-thread overflow-tier slot)? ALWAYS YES, whenever this
//      invocation needed tracking at all -- REGARDLESS of the answer to
//      question 1. An admission-closed invocation is pushed exactly like
//      an inactive marker: dataless, and NOT counted toward the barrier,
//      but still occupying its own position -- see
//      Gate2CAdmission::AdmissionClosed and this file's top-of-file
//      comment for why conflating "no barrier reservation" with "no
//      position" was the flaw a later review found in an earlier version
//      of this function.
//
// (slot, services, moveData) are stored (bounded tier) or just `slot`
// (overflow tier) purely for ResolveFrame's later secondary identity
// assertion -- they play no role in deciding which frame a POST
// resolves; that is always positional.
//
// Both tiers are FIXED CAPACITY (see kGate2CMaxFrameDepth and its
// overflow-tier counterpart in the .cpp) -- neither ever allocates, and
// this holds regardless of how many entries are barrier-participating
// vs. admission-closed placeholders; both kinds consume the same fixed
// capacity. If a single thread ever needs to track more concurrently-
// unresolved invocations than BOTH tiers combined can hold (expected to
// require 16 simultaneously-unresolved ProcessMovement calls on one
// thread -- unreachable in ordinary operation, since real nesting depth
// is expected to be 1, rarely 2, and admission-closed placeholders only
// ever accumulate during the brief window a Start()/Finalize() is
// actively closing), this thread enters a sticky, thread-local "fatal
// depth" state: this invocation is declined (NotTracked; its barrier
// reservation, if any, is undone), EVERY slot currently represented
// anywhere in either tier is immediately poisoned (not just the
// triggering invocation's own slot -- every one of them is equally
// stranded from this point on), and from this point on ResolveFrame
// stops touching either tier or any barrier on this thread entirely --
// see ResolveFrame's comment for why that is still fail-closed even
// though it is not self-recovering. This is thread-scoped, not
// process-wide: other threads' Gate2C tracking is completely unaffected.
Gate2CReservation ReserveFrame(int slot, void* services, void* moveData);

// Called from the PRE hook exactly once, immediately after ReserveFrame
// returns Reserved (never after any other admission value, and never
// more than once per reservation) -- synchronously, before any other
// hook-triggering code runs on this thread, so the reservation this
// completes is always still the top of this thread's bounded-tier
// stack. `hasData=false` commits an inert, non-recording marker (Gate2C
// was inactive for this specific call, i.e. reservation.active was
// false) -- every other parameter is then ignored. `invocationId` must
// be exactly the value ReserveFrame returned; passing anything else is a
// caller bug (asserted internally, since if this thread ever called
// Reserve then Commit out of order, positional correctness for every
// subsequent frame on this thread would already be compromised).
void CommitFrame(int slot, uint64_t invocationId, bool hasData, float preForward, float preSide, bool wroteIntent,
                  float injectedForward, float injectedSide, bool lockAllActive);

enum class Gate2CResolutionOutcome : std::uint8_t
{
    Unmatched,        // nothing was pending on this thread for this call
    NonRecording,     // matched positionally; the entry carries no data (Gate2C was inactive at reserve time)
    Mismatch,         // matched positionally, but the identity assertion failed -- already poisoned; no data
    OverflowResolved, // matched the overflow tier -- no data ever existed for it
    FatalDepth,       // this thread is in the sticky fatal-depth state -- nothing was touched at all
    DataReady         // matched positionally, identity confirmed, real data -- see Gate2CResolution's fields
};

struct Gate2CResolution
{
    Gate2CResolutionOutcome outcome = Gate2CResolutionOutcome::Unmatched;
    // Only meaningful when outcome == DataReady.
    uint32_t generation = 0;
    uint64_t invocationId = 0;
    float preForward = 0, preSide = 0;
    bool wroteIntent = false;
    float injectedForward = 0, injectedSide = 0;
    bool lockAllActive = false;
};

// Called from the POST hook for EVERY real ProcessMovement invocation --
// unconditionally, not gated on IsActiveForSlot (a frame reserved just
// before Finalize ran must still be positionally resolved here, or this
// thread's stack would never rebalance). Resolves (pops) exactly the one
// entry most recently reserved-and-not-yet-resolved on THIS thread --
// strict LIFO by position, never a search keyed by (slot, services,
// moveData). This includes admission-closed placeholder entries (see
// ReserveFrame): they resolve exactly like an inactive marker, via the
// NonRecording outcome below -- there is no separate outcome value for
// them, because from POST's perspective a "dataless, correctly-
// positioned entry" is the same thing regardless of why it carries no
// data.
//
// The outstanding-invocation barrier is decremented ONLY for entries
// that actually reserved one (see ReserveFrame -- admission-closed
// placeholders never did), and always against the SLOT STORED IN THE
// RESOLVED ENTRY (not against this call's own `slot` parameter) -- the
// two are only guaranteed equal when the positional invariant actually
// held, which is exactly what the assertion below checks, so the
// barrier accounting stays correct even in the one case that assertion
// exists to catch.
//
// Deliberately split from committing the final sample (see
// CommitResolvedSample below): this function does NOT take the POST-side
// CMoveData reading, so InputInjector.cpp only has to read CMoveData
// again (a handful of memcpys, cheap but not free at ProcessMovement's
// call frequency) when the outcome is actually DataReady, not on every
// single invocation regardless of outcome.
//
// If the resolved entry came from the overflow tier, only `slot` is
// compared (that tier stores no services/moveData -- see
// Gate2CAdmission::Overflow); a mismatch there is caught with strictly
// less precision than the bounded tier's full 3-field check, but a
// genuine (barrier-participating) overflow entry is already INVALID by
// construction either way (see ReserveFrame's Overflow case), so this is
// a documented, safe scope reduction. If it came from the bounded tier,
// (slot, services, moveData) are compared in full against what
// ReserveFrame stored for it. Either way, a mismatch is treated as
// fail-closed (poisons every slot involved, forces their `active` false,
// records nothing), never silently trusted, because it would mean this
// design's one remaining structural assumption (same-thread PRE/POST
// pairing) did not hold in practice.
//
// If NEITHER tier has anything pending on this thread, nothing was ever
// reserved for this call (e.g. Gate2C was untouched here) -- counted,
// not silently ignored, via the same distinct counter as before.
//
// If this thread has previously hit the fatal-depth condition (see
// ReserveFrame: both bounded tiers exhausted simultaneously -- expected
// to be unreachable in practice, see this file's top-of-file comment),
// this call does nothing at all: no tier is touched, no barrier is
// touched, only a counter increments. This is the one documented case
// this design cannot make both fail-closed AND self-recovering -- it IS
// fail-closed (no data is ever recorded once fatal, nothing is
// positionally guessed at), but the affected slots' outstandingFrames
// can no longer reach zero on their own, so their Start() stays BUSY
// until the process restarts, exactly like a genuinely lost POST (see
// Start()'s comment) -- just reachable through a different, far less
// likely trigger (16 simultaneously-unresolved ProcessMovement
// invocations on one thread).
Gate2CResolution BeginResolveFrame(int slot, void* services, void* moveData);

// Called from the POST hook exactly once, immediately after
// BeginResolveFrame returns DataReady (never after any other outcome).
// Performs the one bounded, atomically-claimed write for this
// invocation, using `resolution`'s stored PRE/INJECTED fields together
// with the POST-side CMoveData reading the caller just took. If the
// generation no longer matches CurrentGeneration(slot) (a Start/Finalize
// happened between this invocation's PRE and this call), the record is
// discarded and counted as a stale-generation drop instead of being
// written.
void CommitResolvedSample(int slot, const Gate2CResolution& resolution, int64_t nowMs, float postForward,
                           float postSide, float velX, float velY, float velZ, float originX, float originY,
                           float originZ);

} // namespace cs2bc::gate2c

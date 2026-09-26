// Standalone, host-side test for the Gate2C-only movement-sink isolation
// logic in src/features/recorder/InputInjector.cpp, as it exists on top of
// commit e7d1b289767a95fc3734df8417ca95c4b09fb53b (branch
// gate2c-diagnostics-v1) plus two small, separately-reviewed fixes applied
// on top of that commit:
//   1. StartUsercmdMovement and StartUsercmdMovementGate2COnly now call
//      ExpireGate2COnlyLocked(slot) before their own exclusivity checks, so
//      a Gate2C-only owner whose deadline has passed but whose slot's hooks
//      have stopped firing (e.g. a killed/kicked bot) cannot indefinitely
//      block a fresh Start on that slot -- e7d1b289 only reaped from
//      GetUsercmdWork/PeekUsercmdMovement/ApplyUsercmdMovement, all of which
//      require the slot's hooks to still be firing.
//   2. UsercmdWork/GetUsercmdWork no longer report a `suppressApply` field,
//      and HookedPlayerRunCommand's call site no longer gates on one --
//      ApplyUsercmdMovement's own g_gate2cOnlyOwners check (made fresh,
//      under the same lock, against the same freshly-read movements.back())
//      is the sole suppression decision point, removing a narrow but real
//      stale-snapshot asymmetry the outer gate could otherwise introduce.
//
// NOT wired into the main plugin build or CI, and NOT built or run as part
// of this change unless separately authorized -- see this directory's
// CMakeLists.txt.
//
// ============================================================================
// IMPORTANT -- READ BEFORE TRUSTING THIS FILE'S RESULTS
// ============================================================================
// Everything below is a MODEL TEST, not a compiled instance of the
// production translation unit, and not proof of the production code's
// thread-safety. InputInjector.cpp cannot be compiled standalone the way
// tests/gate2c/gate2c_lifecycle_test.cpp compiles Gate2CDiagnostics.cpp
// directly: it depends on HL2SDK types, protobuf-generated messages, KHook,
// PawnBinding, MotionRecorder and several sibling modules, none of which are
// engine-independent, and extracting the real per-slot state (g_usercmdMovements,
// g_gate2cOnlyOwners, g_usercmdInjectionMutex, ExpireGate2COnlyLocked,
// StartUsercmdMovement[Gate2COnly], CancelUsercmdMovement, GetUsercmdWork,
// PeekUsercmdMovement, ApplyUsercmdMovement's decision logic) into its own
// engine-independent module so it COULD be compiled and driven directly was
// judged out of scope for this change: it is a real refactor of
// already-CI-validated code, not the kind of small, targeted fix this
// integration covers, and was not part of what was approved. That refactor
// remains a legitimate, separately-approvable follow-up if genuine
// production-code coverage is wanted later.
//
// What this file actually gives you: the SlotState class below reproduces,
// field-for-field and branch-for-branch, the real per-slot algorithm as
// hand-verified against the current InputInjector.cpp source at review
// time (see the line references in each method's comment). It exercises
// the real DECISION LOGIC (exclusivity, ownership, expiry, cancellation)
// with concrete inputs, concrete simulated time, and real concurrent
// std::thread execution against a real std::mutex -- which is meaningfully
// stronger evidence than the repo's other Gate2C-only check
// (tests/gate2c/gate2c_only_contract_test.py, which only greps for
// substrings in the source text and executes no logic at all). But it is
// still, unavoidably, a hand-derived mirror: a mistake in transcribing the
// real algorithm here would not be caught by anything in this file, only by
// a careful side-by-side diff review against the real source (which this
// file's comments are written to make easy) or, eventually, by genuine
// production-code coverage per the paragraph above.
// ============================================================================

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

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

// ---------------------------------------------------------------------
// Mirror model (single slot per instance; every real operation under test
// is already fully slot-scoped, so one instance per test is equivalent and
// simpler to drive deterministically than a kMaxSlots-sized array).
// ---------------------------------------------------------------------

// Mirrors UsercmdMovement (InputInjector.cpp ~line 119): expiresAtMs is
// zero for an ordinary movement, non-zero only for a Gate2C-only session.
struct UsercmdMovement
{
    int64_t id;
    float forwardMove;
    float leftMove;
    int64_t expiresAtMs = 0;
};

// Mirrors Gate2COnlyOwner (~line 142): a bare movement id, zero when unset.
struct Gate2COnlyOwner
{
    int64_t movementId = 0;
};

class SlotState
{
public:
    // Mirrors ExpireGate2COnlyLocked (~line 172, moved earlier by fix #1;
    // logic unchanged from e7d1b289). Caller must hold `mutex_`. Matches by
    // owner_.movementId, so a stale/already-superseded owner can never
    // erase or clear a newer session's entry.
    void ExpireLocked(int64_t nowMs)
    {
        if (!owner_.movementId) return;
        auto it = std::find_if(movements_.begin(), movements_.end(),
                                [&](const UsercmdMovement& m) { return m.id == owner_.movementId; });
        if (it == movements_.end() || (it->expiresAtMs && nowMs >= it->expiresAtMs))
        {
            if (it != movements_.end()) movements_.erase(it);
            owner_ = {};
        }
    }

    // Mirrors StartUsercmdMovement (~line 207), with fix #1: reaps before
    // checking exclusivity, so a stale expired owner self-heals here even
    // if nothing else has touched this slot since it expired.
    int64_t StartUsercmdMovement(int64_t id, float forwardMove, float leftMove, int64_t nowMs)
    {
        std::scoped_lock lock(mutex_);
        ExpireLocked(nowMs);
        if (owner_.movementId) return -1;
        movements_.push_back({ id, forwardMove, leftMove, 0 });
        return id;
    }

    // Mirrors StartUsercmdMovementGate2COnly (~line 223), with fix #1.
    // Rejects unless the movement vector is completely empty (the real
    // file's stricter admission gate: a Gate2C-only session can only ever
    // be the SOLE entry, so ApplyUsercmdMovement never has to decide which
    // of several entries the owner refers to).
    int64_t StartUsercmdMovementGate2COnly(int64_t id, float forwardMove, float leftMove, int64_t maxDurationMs, int64_t nowMs)
    {
        if (maxDurationMs < 1000 || maxDurationMs > 60000) return -1;
        std::scoped_lock lock(mutex_);
        ExpireLocked(nowMs);
        if (!movements_.empty() || owner_.movementId) return -1;
        movements_.push_back({ id, forwardMove, leftMove, nowMs + maxDurationMs });
        owner_ = { id };
        return id;
    }

    // Mirrors CancelUsercmdMovement (~line 254): erase by id, clear owner
    // iff its movementId matches the cancelled id.
    bool CancelUsercmdMovement(int64_t movementId)
    {
        std::scoped_lock lock(mutex_);
        auto it = std::find_if(movements_.begin(), movements_.end(),
                                [&](const UsercmdMovement& m) { return m.id == movementId; });
        if (it == movements_.end()) return false;
        movements_.erase(it);
        if (owner_.movementId == movementId) owner_ = {};
        return true;
    }

    // Mirrors GetUsercmdWork's `movement` field (~line 360): reap, then
    // report emptiness. No suppression field is reported here (fix #2) --
    // the real suppression decision lives solely in WouldApply below.
    bool HasUsercmdMovement(int64_t nowMs)
    {
        std::scoped_lock lock(mutex_);
        ExpireLocked(nowMs);
        return !movements_.empty();
    }

    // Mirrors PeekUsercmdMovement (Gate2C's own read: reaps, then reads
    // .back() unconditionally, never gated by ownership).
    std::optional<UsercmdMovement> Peek(int64_t nowMs)
    {
        std::scoped_lock lock(mutex_);
        ExpireLocked(nowMs);
        if (movements_.empty()) return std::nullopt;
        return movements_.back();
    }

    // Mirrors ApplyUsercmdMovement's decision (~line 397-402, post-fix #2):
    // the SOLE suppression decision point. Reap, then check ownership
    // BEFORE reading .back() -- matches the real file's ordering exactly.
    bool WouldApply(int64_t nowMs)
    {
        std::scoped_lock lock(mutex_);
        ExpireLocked(nowMs);
        if (owner_.movementId) return false;
        if (movements_.empty()) return false;
        return true;
    }

    // Mirrors ClearUsercmdInjections/Remove()'s unconditional reset.
    void ClearAll()
    {
        std::scoped_lock lock(mutex_);
        movements_.clear();
        owner_ = {};
    }

    size_t MovementCount()
    {
        std::scoped_lock lock(mutex_);
        return movements_.size();
    }

private:
    std::mutex mutex_;
    std::vector<UsercmdMovement> movements_;
    Gate2COnlyOwner owner_;
};

constexpr int64_t kMinDurationMs = 1000;
constexpr int64_t kMaxDurationMs = 60000;

// ---------------------------------------------------------------------
// 1. Default Gate2B behavior: an ordinary session is entirely unaffected.
// ---------------------------------------------------------------------
void TestDefaultGate2BBehaviorUnaffected()
{
    SlotState slot;
    int64_t id = slot.StartUsercmdMovement(1, 1.0F, 0.0F, /*nowMs=*/0);
    CHECK(id == 1);
    CHECK(slot.HasUsercmdMovement(0));
    CHECK(slot.WouldApply(0));
    auto peeked = slot.Peek(0);
    CHECK(peeked.has_value() && peeked->id == 1);
    CHECK(slot.CancelUsercmdMovement(1));
    CHECK(!slot.HasUsercmdMovement(0));
    CHECK(!slot.WouldApply(0));
}

// ---------------------------------------------------------------------
// 2. Suppressed start and visibility: Gate2C-only start is visible to
//    HasUsercmdMovement/Peek but WouldApply is false, atomically from the
//    very first observation.
// ---------------------------------------------------------------------
void TestSuppressedStartAndVisibility()
{
    SlotState slot;
    int64_t id = slot.StartUsercmdMovementGate2COnly(1, 1.0F, 0.0F, kMaxDurationMs, /*nowMs=*/0);
    CHECK(id == 1);
    CHECK(slot.HasUsercmdMovement(0));
    auto peeked = slot.Peek(0);
    CHECK(peeked.has_value() && peeked->id == 1 && peeked->forwardMove == 1.0F);
    CHECK(!slot.WouldApply(0)); // the crux: visible to Gate2C, suppressed for Gate2B
}

// ---------------------------------------------------------------------
// 3. Exclusive ownership: while a Gate2C-only owner is active, neither an
//    ordinary StartUsercmdMovement nor a second StartUsercmdMovementGate2COnly
//    may create a competing session. The real file's admission gate is
//    stricter than "no owner": it requires the vector to be entirely empty,
//    so even calling StartUsercmdMovement first (creating an ordinary
//    entry) must block a subsequent Gate2C-only start.
// ---------------------------------------------------------------------
void TestExclusiveOwnership()
{
    SlotState slot;
    int64_t ownerId = slot.StartUsercmdMovementGate2COnly(1, 1.0F, 0.0F, kMaxDurationMs, 0);
    CHECK(ownerId == 1);

    CHECK(slot.StartUsercmdMovement(2, 0.5F, 0.5F, 0) == -1);
    CHECK(slot.StartUsercmdMovementGate2COnly(3, 0.5F, 0.5F, kMaxDurationMs, 0) == -1);
    CHECK(slot.MovementCount() == 1);
    CHECK(!slot.WouldApply(0));

    CHECK(slot.CancelUsercmdMovement(ownerId));
    CHECK(slot.MovementCount() == 0);

    // No Gate2C-only owner exists now -- ordinary behavior is preserved.
    int64_t id = slot.StartUsercmdMovement(4, 1.0F, 0.0F, 0);
    CHECK(id == 4);
    CHECK(slot.WouldApply(0));

    // The stricter empty-vector gate: an ordinary entry alone (no owner)
    // must also block a Gate2C-only start.
    CHECK(slot.StartUsercmdMovementGate2COnly(5, 1.0F, 0.0F, kMaxDurationMs, 0) == -1);
}

// ---------------------------------------------------------------------
// 4. Stale cancellation: a delayed/duplicate cancel for an id that no
//    longer owns this slot must be a no-op against a newer session.
// ---------------------------------------------------------------------
void TestStaleCancellationCannotClearNewerOwner()
{
    SlotState slot;
    int64_t oldId = slot.StartUsercmdMovementGate2COnly(1, 1.0F, 0.0F, kMaxDurationMs, 0);
    CHECK(slot.CancelUsercmdMovement(oldId));
    int64_t newId = slot.StartUsercmdMovementGate2COnly(2, 1.0F, 0.0F, kMaxDurationMs, 0);
    CHECK(newId == 2);

    CHECK(!slot.CancelUsercmdMovement(oldId)); // no-op: old id no longer present
    CHECK(!slot.WouldApply(0));                // new session's ownership intact
    auto peeked = slot.Peek(0);
    CHECK(peeked.has_value() && peeked->id == newId);
}

// ---------------------------------------------------------------------
// 5. Repeated start/cancel cycles leave the slot clean each time.
// ---------------------------------------------------------------------
void TestRepeatedStartCancelCycles()
{
    SlotState slot;
    for (int64_t i = 1; i <= 5; ++i)
    {
        int64_t id = slot.StartUsercmdMovementGate2COnly(i, 1.0F, 0.0F, kMaxDurationMs, 0);
        CHECK(id == i);
        CHECK(!slot.WouldApply(0));
        CHECK(slot.CancelUsercmdMovement(id));
        CHECK(slot.MovementCount() == 0);
    }
    int64_t ordinaryId = slot.StartUsercmdMovement(100, 1.0F, 0.0F, 0);
    CHECK(slot.WouldApply(0));
    CHECK(slot.CancelUsercmdMovement(ordinaryId));
}

// ---------------------------------------------------------------------
// 6. Expiry: bounded and lazy, reaped on the next relevant call, not by an
//    independent timer.
// ---------------------------------------------------------------------
void TestExpiryBoundedAndLazy()
{
    SlotState slot;
    int64_t id = slot.StartUsercmdMovementGate2COnly(1, 1.0F, 0.0F, kMaxDurationMs, /*nowMs=*/0);
    CHECK(id == 1);

    CHECK(slot.HasUsercmdMovement(kMaxDurationMs - 1));
    CHECK(!slot.WouldApply(kMaxDurationMs - 1));

    CHECK(!slot.HasUsercmdMovement(kMaxDurationMs)); // deadline reached: reaped
    CHECK(slot.MovementCount() == 0);

    int64_t newId = slot.StartUsercmdMovementGate2COnly(2, 1.0F, 0.0F, kMaxDurationMs, kMaxDurationMs);
    CHECK(newId == 2);
    CHECK(!slot.WouldApply(kMaxDurationMs));

    // Expired-but-not-yet-reaped cannot clear a NEWER owner: the reap
    // inside the new session's own Start call handles this before its
    // exclusivity check runs, so the new owner is never touched by the
    // old one's belated expiry.
    SlotState slot2;
    int64_t a = slot2.StartUsercmdMovementGate2COnly(10, 1.0F, 0.0F, kMaxDurationMs, 0);
    CHECK(a == 10);
    int64_t muchLater = kMaxDurationMs + 1000;
    int64_t b = slot2.StartUsercmdMovementGate2COnly(11, 1.0F, 0.0F, kMaxDurationMs, muchLater);
    CHECK(b == 11);
    CHECK(!slot2.WouldApply(muchLater));
    auto peeked = slot2.Peek(muchLater);
    CHECK(peeked.has_value() && peeked->id == 11);
}

// ---------------------------------------------------------------------
// 7. Regression: fix #1. Start after expiry with NO intervening call to
//    GetUsercmdWork/Peek/ApplyUsercmdMovement -- i.e. the slot's hooks have
//    stopped firing entirely (dead/kicked bot) and nothing has reaped the
//    stale owner yet. Before fix #1 (e7d1b289 unmodified), neither Start
//    function reaped expired ownership itself, so this exact call would
//    have returned -1 forever. With the fix, Start's own reap call closes
//    the gap: a fresh session is accepted immediately once the old one's
//    deadline has passed, even with zero intervening hook activity.
// ---------------------------------------------------------------------
void TestStartAfterExpiryWithNoInterveningHooks()
{
    SlotState slot;
    int64_t id = slot.StartUsercmdMovementGate2COnly(1, 1.0F, 0.0F, kMinDurationMs, /*nowMs=*/0);
    CHECK(id == 1);

    int64_t afterDeadline = kMinDurationMs; // exactly at the deadline
    // No HasUsercmdMovement/Peek/WouldApply call in between -- go straight
    // to a fresh Start, simulating a slot whose hooks never fired again.
    int64_t freshId = slot.StartUsercmdMovementGate2COnly(2, 1.0F, 0.0F, kMaxDurationMs, afterDeadline);
    CHECK(freshId == 2);
    CHECK(slot.MovementCount() == 1);

    // Same for the ordinary Start path.
    SlotState slot2;
    int64_t ownerId = slot2.StartUsercmdMovementGate2COnly(1, 1.0F, 0.0F, kMinDurationMs, 0);
    CHECK(ownerId == 1);
    int64_t ordinaryId = slot2.StartUsercmdMovement(2, 1.0F, 0.0F, kMinDurationMs);
    CHECK(ordinaryId == 2); // would be -1 without fix #1
}

// ---------------------------------------------------------------------
// 8. Concurrent access: one writer thread repeatedly starts/cancels
//    Gate2C-only sessions while reader threads continuously call
//    Peek/WouldApply/HasUsercmdMovement. WouldApply must never desync from
//    "empty" -- checked using the SAME real std::mutex discipline the
//    production file uses (one lock, decision made while holding it). This
//    demonstrates the model's own locking is internally consistent; it is
//    NOT evidence about the real translation unit's thread-safety (see the
//    file-level disclaimer above) -- InputInjector.cpp's actual hooks are
//    driven by the CS2 engine's own worker threads, which this model does
//    not and cannot reproduce.
// ---------------------------------------------------------------------
void TestConcurrentAccessNoTornObservations()
{
    SlotState slot;
    std::atomic<bool> stop{ false };
    std::atomic<int> inconsistencies{ 0 };
    std::atomic<int64_t> nextId{ 1 };

    auto writer = [&] {
        while (!stop.load(std::memory_order_relaxed))
        {
            int64_t id = nextId.fetch_add(1, std::memory_order_relaxed);
            int64_t got = slot.StartUsercmdMovementGate2COnly(id, 1.0F, 0.0F, kMaxDurationMs, 0);
            if (got == id) slot.CancelUsercmdMovement(id);
        }
    };

    auto reader = [&] {
        while (!stop.load(std::memory_order_relaxed))
        {
            auto peeked = slot.Peek(0);
            bool wouldApply = slot.WouldApply(0);
            if (wouldApply && !peeked.has_value()) inconsistencies.fetch_add(1, std::memory_order_relaxed);
        }
    };

    std::thread w(writer);
    std::vector<std::thread> readers;
    for (int i = 0; i < 4; ++i) readers.emplace_back(reader);

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    stop.store(true, std::memory_order_relaxed);
    w.join();
    for (auto& t : readers) t.join();

    CHECK(inconsistencies.load() == 0);
}

// ---------------------------------------------------------------------
// 9. Cleanup and unload: ClearAll removes an active session and its
//    ownership regardless of bookkeeping, and the slot starts clean after.
// ---------------------------------------------------------------------
void TestCleanupAndUnload()
{
    SlotState slot;
    slot.StartUsercmdMovementGate2COnly(1, 1.0F, 0.0F, kMaxDurationMs, 0);
    CHECK(slot.MovementCount() == 1);
    slot.ClearAll();
    CHECK(slot.MovementCount() == 0);
    CHECK(slot.WouldApply(0) == false);

    int64_t id = slot.StartUsercmdMovement(2, 1.0F, 0.0F, 0);
    CHECK(id == 2);
    CHECK(slot.WouldApply(0));
}

} // namespace

int main()
{
    TestDefaultGate2BBehaviorUnaffected();
    TestSuppressedStartAndVisibility();
    TestExclusiveOwnership();
    TestStaleCancellationCannotClearNewerOwner();
    TestRepeatedStartCancelCycles();
    TestExpiryBoundedAndLazy();
    TestStartAfterExpiryWithNoInterveningHooks();
    TestConcurrentAccessNoTornObservations();
    TestCleanupAndUnload();

    std::printf("%d/%d checks passed\n", g_checks - g_failures, g_checks);
    return g_failures == 0 ? 0 : 1;
}

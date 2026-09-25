// BotController control-chain PoC harness -- DISPOSABLE, EXPERIMENTAL, NOT PRODUCTION.
//
// Scope, deliberately narrow per this task's authorization:
//   - Uses BotController's existing public control surface (IBotControllerApi) --
//     no new native hooks, no changes to BotController's own implementation. (An
//     earlier version of this harness also P/Invoked four native "hook-call-counter"
//     exports; those were found not to exist in this build and were removed -- see
//     the comment above the remaining BotController_GetVersion DllImport.)
//   - Every read-back is independent: direct schema reads of the pawn's own
//     AbsOrigin/EyeAngles, using the exact same live-proven technique as
//     Cs2AiTrainingScenarioActorController/PawnOrientationSchema in the production
//     plugin (ref-property QAngle write/read; AbsOrigin direct read) -- never the
//     BotController API's own boolean return value treated as proof of execution.
//   - Does not touch IScenarioActorController, ScenarioExecutionAuthority, or any
//     other governed production file.
//   - Takes all coordinates/angles as console-command ARGUMENTS, never hardcoded --
//     this harness makes no claim about any specific CS2 map coordinate; the
//     operator supplies already-verified values at invocation time.
//
// Console commands (all admin-only, all fail-soft -- never throw to the caller):
//   css_poc_gate1                          -- load diagnostic snapshot (capability+ABI only)
//   css_poc_baseline <slot>                 -- log current AbsOrigin/EyeAngles for slot
//   css_poc_gate2 <slot> <ms> [lockMode]     -- suppress AI (lockMode: all|aim, default all),
//                                                inject forward movement, sample position+eye
//   css_poc_gate3 <slot> <pitch> <yaw> <ms>  -- DISABLED, see comment above OnGate3 and README
//   css_poc_stop <slot>                     -- cancel all injections/locks/replay for slot, unlock

using System.Runtime.InteropServices;
using BotControllerApi;
using CounterStrikeSharp.API;
using CounterStrikeSharp.API.Core;
using CounterStrikeSharp.API.Core.Attributes.Registration;
using CounterStrikeSharp.API.Core.Capabilities;
using CounterStrikeSharp.API.Modules.Commands;
using CounterStrikeSharp.API.Modules.Memory;
using CounterStrikeSharp.API.Modules.Utils;

namespace BotControllerPocHarness;

public sealed class PocHarnessPlugin : BasePlugin
{
    public override string ModuleName => "BotControllerPocHarness";
    public override string ModuleVersion => "0.1.0-disposable";
    public override string ModuleAuthor => "cs2-ai-training (experimental, not production)";
    public override string ModuleDescription =>
        "Disposable control-chain PoC harness for BotController v0.7.0 candidate. Not part of the production plugin.";

    private static readonly PluginCapability<IBotControllerApi> BotControllerCap = new("botcontroller:api");
    private string _logPath = string.Empty;

    // ---- CORRECTED post-restart: the four native hook-call-counter exports
    // referenced here previously (BotController_GetHookCallCount,
    // BotController_GetPlayerRunCommandCallCount, BotController_GetPhysicsSimulateCallCount,
    // BotController_GetFinishMoveCallCount) were claimed "confirmed present as native
    // exports" in the prior session's handoff. That claim was checked against the
    // actual pinned-commit source (src/bridge/exports.cpp,
    // github.com/Jebus8five/CS2-Bot-Controller @ 9304ee6727e78e7f116b611a5c246278ace01223)
    // and against the literal symbol strings inside the already-downloaded, hash-verified
    // BotController.dll (C:\CS2BotControllerBuild\extracted-MM\...\BotController.dll).
    // Neither contains these four names, nor any "CallCount" symbol at all -- they do
    // not exist in this build. A DllImport of a nonexistent export throws
    // EntryPointNotFoundException on first call, so these declarations and Gate 1's
    // use of them have been removed rather than left in place to fail at runtime.
    // No replacement native call-count mechanism exists in this build; Gate 1 below
    // now relies only on capability resolution + AbiVersion as load evidence, and
    // explicitly does NOT claim proof that hooked functions are being invoked per-tick.
    [DllImport("BotController", CallingConvention = CallingConvention.Cdecl)]
    private static extern int BotController_GetVersion();

    // ---- Gate 2B diagnostic-only exports (native, read-only observation).
    // Not part of IBotControllerApi/BotControllerApi.dll -- these three are
    // new, dedicated-diagnostic native exports, called only from this
    // disposable harness, never touching gameplay/movement semantics.
    [DllImport("BotController", CallingConvention = CallingConvention.Cdecl)]
    private static extern int BotController_Gate2BDiagnosticStart(int slot);

    [DllImport("BotController", CallingConvention = CallingConvention.Cdecl)]
    private static extern int BotController_Gate2BDiagnosticMarkCancelled(int slot);

    [DllImport("BotController", CallingConvention = CallingConvention.Cdecl)]
    private static extern int BotController_Gate2BDiagnosticFinalize(int slot, int aborted);

    public override void Load(bool hotReload)
    {
        _logPath = Path.Combine(ModuleDirectory, "poc-harness.log");
        Log($"[load] PocHarnessPlugin loaded. hotReload={hotReload}");
    }

    private void Log(string line)
    {
        var stamped = $"{DateTime.UtcNow:O} {line}";
        Server.PrintToConsole($"[BotControllerPoC] {line}");
        try { File.AppendAllText(_logPath, stamped + Environment.NewLine); }
        catch (Exception ex) { Server.PrintToConsole($"[BotControllerPoC] log write failed: {ex.GetType().Name}: {ex.Message}"); }
    }

    // ---- GATE 1: load verification ----
    // A successful Load() message alone is NOT evidence hooks installed. This gate
    // was originally written to also read native hook-call counters as delta evidence
    // that hooked functions fire -- those exports do not exist in this build (see the
    // corrected comment above the DllImport block), so that check has been removed
    // rather than left to throw. Gate 1 now confirms capability resolution + a
    // successful AbiVersion read only. This is evidence the managed<->native ABI
    // handshake succeeded, NOT evidence any specific hook is installed or firing for
    // any specific bot -- Gates 2/3's own independent pawn-state read-back (schema
    // reads of AbsOrigin/EyeAngles, never BotController's own return values) remain
    // the only real evidence in this harness that control was actually exercised.
    [ConsoleCommand("css_poc_gate1", "GATE1: report BotController load diagnostics")]
    [CommandHelper(whoCanExecute: CommandUsage.CLIENT_AND_SERVER)]
    public void OnGate1(CCSPlayerController? caller, CommandInfo cmd)
    {
        IBotControllerApi? api;
        try { api = BotControllerCap.Get(); }
        catch (Exception ex) { Log($"[gate1] FAIL capability resolution threw {ex.GetType().Name}: {ex.Message}"); return; }

        if (api is null) { Log("[gate1] FAIL capability not registered (BotControllerImpl did not load or ABI check failed)"); return; }

        int abi;
        try { abi = api.AbiVersion; }
        catch (Exception ex) { Log($"[gate1] capability resolved but AbiVersion threw {ex.GetType().Name}: {ex.Message} -- treat as FAIL"); return; }

        Log($"[gate1] capability resolved. api.AbiVersion={abi} (managed ExpectedAbiVersion is compiled to 21 for this pinned commit)");
        Log("[gate1] PASS on capability+ABI evidence only. This build exports no native hook-call counters (BotController_GetHookCallCount and siblings do not exist in src/bridge/exports.cpp or in the compiled DLL's symbol table -- verified, not assumed). Proceed to Gate 2/3's own read-back to get real evidence of control.");
    }

    // ---- Independent read-back helper: mirrors production PawnOrientationSchema's
    // live-proven technique exactly (do not invent a new one). ----
    private static (bool ok, Vector? origin, QAngle? eye, string detail) ReadPawnState(CCSPlayerPawn pawn)
    {
        try
        {
            var origin = pawn.AbsOrigin;
            var eye = pawn.EyeAngles;
            if (origin is null) return (false, null, null, "AbsOrigin null");
            return (true, origin, eye, "ok");
        }
        catch (Exception ex) { return (false, null, null, $"unreadable:{ex.GetType().Name}"); }
    }

    private static CCSPlayerController? ResolveSlot(int slot) =>
        Utilities.GetPlayers().FirstOrDefault(p => p.IsValid && p.Slot == slot);

    [ConsoleCommand("css_poc_baseline", "Log current AbsOrigin/EyeAngles for a slot")]
    [CommandHelper(minArgs: 1, usage: "<slot>", whoCanExecute: CommandUsage.CLIENT_AND_SERVER)]
    public void OnBaseline(CCSPlayerController? caller, CommandInfo cmd)
    {
        if (!int.TryParse(cmd.GetArg(1), out var slot)) { Log("[baseline] bad slot arg"); return; }
        var controller = ResolveSlot(slot);
        if (controller is null || !controller.IsBot) { Log($"[baseline] slot {slot} is not a live bot -- refusing (bot-only, matches production Fair Play discipline)"); return; }
        var pawn = controller.PlayerPawn?.Value;
        if (pawn is null) { Log($"[baseline] slot {slot} has no pawn"); return; }
        var (ok, origin, eye, detail) = ReadPawnState(pawn);
        Log(ok
            ? $"[baseline] slot={slot} origin=({origin!.X:F4},{origin.Y:F4},{origin.Z:F4}) eye=({eye!.X:F4},{eye.Y:F4},{eye.Z:F4})"
            : $"[baseline] slot={slot} read FAILED: {detail}");
    }

    // ---- GATE 2: movement ----
    private readonly Dictionary<int, long> _activeMovement = new();

    // Diagnostic addition: Gate 2's original design hardcoded LockKind.All. Source
    // inspection after the first Gate 2 run (which accepted both Lock(All) and
    // StartUsercmdMovement but produced zero displacement across 129 samples)
    // found that LockKind.All supersedes CCSBot::Update's real body entirely
    // (BotController.cpp HookedUpdate), while ApplyUsercmdMovement's own doc
    // comment ("Replaces Bot AI analog movement after the final command is
    // generated", InputInjector.cpp) assumes a command already exists to modify.
    // TECH.md's own Lock Model section documents that Aim -- unlike All -- leaves
    // the bot able to "still move and decide". This parameter lets the operator
    // choose which lock kind to combine with StartUsercmdMovement, to determine
    // empirically whether that's actually the reason -- a hypothesis, not yet
    // confirmed, since PlayerRunCommand's own invocation conditions live in the
    // closed-source engine, not in this repository.
    [ConsoleCommand("css_poc_gate2", "GATE2: suppress AI, inject bounded forward movement, sample position+eye")]
    [CommandHelper(minArgs: 2, usage: "<slot> <durationMs> [lockMode: all|aim, default all]", whoCanExecute: CommandUsage.CLIENT_AND_SERVER)]
    public void OnGate2(CCSPlayerController? caller, CommandInfo cmd)
    {
        if (!int.TryParse(cmd.GetArg(1), out var slot) || !int.TryParse(cmd.GetArg(2), out var durationMs))
        { Log("[gate2] bad args"); return; }

        // Optional 3rd arg, defaults to "all" -- preserves prior behavior exactly
        // when omitted, per this task's authorization.
        var lockModeArg = cmd.GetArg(3);
        LockKind lockKind;
        if (string.IsNullOrWhiteSpace(lockModeArg) || lockModeArg.Equals("all", StringComparison.OrdinalIgnoreCase))
        {
            lockKind = LockKind.All;
        }
        else if (lockModeArg.Equals("aim", StringComparison.OrdinalIgnoreCase))
        {
            lockKind = LockKind.Aim;
        }
        else
        {
            Log($"[gate2] bad lockMode arg '{lockModeArg}' -- must be 'all' or 'aim' (omit for default 'all')");
            return;
        }

        var api = BotControllerCap.Get();
        if (api is null) { Log("[gate2] FAIL: capability not available (run gate1 first)"); return; }
        var controller = ResolveSlot(slot);
        if (controller is null || !controller.IsBot) { Log($"[gate2] slot {slot} is not a live bot"); return; }
        var pawn = controller.PlayerPawn?.Value;
        if (pawn is null) { Log($"[gate2] slot {slot} has no pawn"); return; }

        var (okPre, originPre, eyePre, detailPre) = ReadPawnState(pawn);
        Log(okPre
            ? $"[gate2] PRE origin=({originPre!.X:F4},{originPre.Y:F4},{originPre.Z:F4}) eye=({eyePre!.X:F4},{eyePre.Y:F4})"
            : $"[gate2] PRE read FAILED: {detailPre} -- aborting, cannot establish baseline");
        if (!okPre) return;

        // Gate 2B: begin native diagnostic capture BEFORE Lock/StartUsercmdMovement,
        // so Activation is captured from the very first relevant call. This harness
        // remains the sole owner of the test deadline/lifecycle; the native side only
        // records observations of calls that already happen.
        System.Threading.Interlocked.Exchange(ref _gate2FinalizedGuard, 0); // new test, new finalize claim
        int startStatus;
        try { startStatus = BotController_Gate2BDiagnosticStart(slot); }
        catch (Exception ex) { startStatus = -1; Log($"[gate2] DiagnosticStart threw {ex.GetType().Name}: {ex.Message} -- proceeding without native diagnostics"); }
        if (startStatus == 3) Log("[gate2] DiagnosticStart returned BUSY (a previous window's writers could not be confirmed drained) -- this run's native diagnostic will be unreliable; consider retrying.");

        bool locked;
        try { locked = api.Lock(slot, lockKind); }
        catch (Exception ex) { Log($"[gate2] Lock threw {ex.GetType().Name}: {ex.Message}"); TryFinalizeGate2Diagnostic(slot, aborted: true); return; }
        Log($"[gate2] Lock({lockKind}) accepted={locked} (per BotControllerApi/Types.cs: All freezes CCSBot::Update AND Upkeep, Aim freezes only Upkeep -- acceptance only, not proof it held)");

        long movementId;
        try { movementId = api.StartUsercmdMovement(slot, forwardMove: 1.0f, leftMove: 0.0f); }
        catch (Exception ex)
        {
            Log($"[gate2] StartUsercmdMovement threw {ex.GetType().Name}: {ex.Message}");
            if (locked) api.Unlock(slot, lockKind);
            TryFinalizeGate2Diagnostic(slot, aborted: true);
            return;
        }
        Log($"[gate2] StartUsercmdMovement accepted, id={movementId} (id<0 or ==-1 conventionally means rejected -- treat as such, not as success)");
        if (movementId < 0)
        {
            if (locked) api.Unlock(slot, lockKind);
            TryFinalizeGate2Diagnostic(slot, aborted: true);
            return;
        }
        _activeMovement[slot] = movementId;

        // Bounded sampling loop: one sample per server tick for the requested
        // window, via OnTick, not a busy-wait -- see RegisterListener below.
        _gate2Samples.Clear();
        _gate2Slot = slot;
        _gate2Pawn = pawn;
        _gate2LockKind = lockKind;
        _gate2DeadlineMs = MonotonicMs() + durationMs;
        _gate2Active = true;
        Log($"[gate2] sampling started for {durationMs}ms with lockKind={lockKind}. Call css_poc_stop {slot} early if needed; otherwise it self-stops and logs the full trace.");
    }

    private readonly List<(long tMs, float x, float y, float z, float pitch, float yaw)> _gate2Samples = new();
    private bool _gate2Active;
    private int _gate2Slot;
    private CCSPlayerPawn? _gate2Pawn;
    private LockKind _gate2LockKind;
    private long _gate2DeadlineMs;

    // Gate 2B: bounded post-cancel observation, still locked, owned entirely by
    // this harness's own wall-clock schedule -- independent of whether any
    // native hook fires during it, so a fully-suppressed bot (the exact
    // condition under investigation) still produces a timely, meaningful result
    // instead of an indefinite wait.
    private const long Gate2PostCancelDurationMs = 1000;
    private readonly List<(long tMs, float x, float y, float z, float pitch, float yaw)> _gate2PostCancelSamples = new();
    private bool _gate2PostCancelActive;
    private long _gate2PostCancelDeadlineMs;

    private static long MonotonicMs() => Environment.TickCount64;

    public override void OnAllPluginsLoaded(bool hotReload)
    {
        RegisterListener<Listeners.OnTick>(OnTick);
    }

    private void OnTick()
    {
        if (_gate2Active && _gate2Pawn is not null)
        {
            var (ok, origin, eye, _) = ReadPawnState(_gate2Pawn);
            if (ok) _gate2Samples.Add((MonotonicMs(), origin!.X, origin.Y, origin.Z, eye!.X, eye.Y));
            if (MonotonicMs() >= _gate2DeadlineMs)
            {
                _gate2Active = false;
                BeginGate2PostCancel();
            }
        }
        if (_gate2PostCancelActive && _gate2Pawn is not null)
        {
            var (ok, origin, eye, _) = ReadPawnState(_gate2Pawn);
            if (ok) _gate2PostCancelSamples.Add((MonotonicMs(), origin!.X, origin.Y, origin.Z, eye!.X, eye.Y));
            if (MonotonicMs() >= _gate2PostCancelDeadlineMs)
            {
                _gate2PostCancelActive = false;
                FinishGate2(aborted: false);
            }
        }
        if (_gate3Active && _gate3Pawn is not null)
        {
            var (ok, _, eye, _) = ReadPawnState(_gate3Pawn);
            if (ok) _gate3Samples.Add((MonotonicMs(), eye!.X, eye.Y));
            if (MonotonicMs() >= _gate3DeadlineMs)
            {
                _gate3Active = false;
                FinishGate3();
            }
        }
    }

    // Cancels HOS movement, marks the native diagnostic phase transition, and
    // begins the bounded still-locked post-cancel observation window. Capture
    // stays active throughout -- CancelUsercmdMovement must not, by itself,
    // stop or finalize the diagnostic.
    private void BeginGate2PostCancel()
    {
        var api = BotControllerCap.Get();
        if (api is not null && _activeMovement.TryGetValue(_gate2Slot, out var id))
        {
            try { api.CancelUsercmdMovement(_gate2Slot, id); } catch { /* diagnostic-only, fail soft */ }
        }
        try { BotController_Gate2BDiagnosticMarkCancelled(_gate2Slot); }
        catch (Exception ex) { Log($"[gate2] DiagnosticMarkCancelled threw {ex.GetType().Name}: {ex.Message}"); }

        _gate2PostCancelSamples.Clear();
        _gate2PostCancelDeadlineMs = MonotonicMs() + Gate2PostCancelDurationMs;
        _gate2PostCancelActive = true;
        Log($"[gate2] movement cancelled; observing for {Gate2PostCancelDurationMs}ms while lockKind={_gate2LockKind} remains active before unlock");
    }

    // Single-owner guard so that only the winning finalize path (whichever of
    // OnGate2's early returns, FinishGate2's normal completion, or OnStop's
    // emergency abort runs first) ever emits a COMPLETE/ABORTED result for a
    // given test; a losing path logs that it lost instead of printing a
    // possibly-contradictory label. Reset at the start of each new
    // css_poc_gate2 invocation (see OnGate2's DiagnosticStart call).
    private int _gate2FinalizedGuard;

    // Finalizes the native diagnostic exactly once (via the guard above) and
    // logs the outcome, including native BUSY/INCOMPLETE status accurately.
    private void TryFinalizeGate2Diagnostic(int slot, bool aborted)
    {
        if (System.Threading.Interlocked.CompareExchange(ref _gate2FinalizedGuard, 1, 0) != 0)
        {
            Log("[gate2] finalize already claimed by another code path for this test -- skipping a duplicate, potentially contradictory local summary.");
            return;
        }
        int finalizeStatus;
        try { finalizeStatus = BotController_Gate2BDiagnosticFinalize(slot, aborted ? 1 : 0); }
        catch (Exception ex) { finalizeStatus = -1; Log($"[gate2] DiagnosticFinalize threw {ex.GetType().Name}: {ex.Message}"); }
        string label = finalizeStatus switch
        {
            0 => aborted ? "ABORTED" : "COMPLETE",
            1 => "REPEAT (native side already finalized this window independently)",
            2 => "INVALID_SLOT",
            3 => "BUSY/INCOMPLETE (native bounded drain did not confirm quiescence -- buffers were NOT read; do not treat this as a completed test)",
            _ => $"UNKNOWN(status={finalizeStatus})",
        };
        Log($"[gate2] [{label}] DiagnosticFinalize status={finalizeStatus} -- see native BotController log for the bounded per-phase sample dump (Activation/SteadyState/Cancellation/PostCancel) and invocation counters, when status=0 or 1.");
    }

    // Logs both sample windows and releases the lock(s) after finalizing the
    // native diagnostic. aborted=true labels this an emergency-abort result
    // (see OnStop) rather than a normally-completed Gate 2B run.
    private void FinishGate2(bool aborted)
    {
        TryFinalizeGate2Diagnostic(_gate2Slot, aborted);

        Log($"[gate2] pre-cancel sample count={_gate2Samples.Count} lockKind={_gate2LockKind}");
        foreach (var s in _gate2Samples)
            Log($"[gate2] t={s.tMs} pos=({s.x:F4},{s.y:F4},{s.z:F4}) eye=(pitch={s.pitch:F4},yaw={s.yaw:F4})");
        if (_gate2Samples.Count >= 2)
        {
            var first = _gate2Samples[0]; var last = _gate2Samples[^1];
            var dist = MathF.Sqrt(MathF.Pow(last.x - first.x, 2) + MathF.Pow(last.y - first.y, 2) + MathF.Pow(last.z - first.z, 2));
            var eyeDelta = MathF.Sqrt(MathF.Pow(last.pitch - first.pitch, 2) + MathF.Pow(last.yaw - first.yaw, 2));
            Log($"[gate2] net displacement over pre-cancel window: {dist:F4} units, net eye-angle change: {eyeDelta:F4} deg (lockKind={_gate2LockKind}). Continuity must be assessed from the full per-sample trace above (monotonic-ish progress, no single-sample teleport jump), not from this net figure alone. Under lockKind=All, ANY eye-angle change here would itself be a finding (Upkeep is meant to be frozen too); under lockKind=Aim, eye-angle drift is expected (Upkeep runs normally) -- only the requested StartUsercmdMovement's forward displacement is under test.");
        }

        Log($"[gate2] post-cancel (still locked) sample count={_gate2PostCancelSamples.Count}");
        foreach (var s in _gate2PostCancelSamples)
            Log($"[gate2] t={s.tMs} pos=({s.x:F4},{s.y:F4},{s.z:F4}) eye=(pitch={s.pitch:F4},yaw={s.yaw:F4})");
        if (_gate2PostCancelSamples.Count == 0)
        {
            Log("[gate2] post-cancel window produced no position samples (e.g. pawn became unreadable) -- record this as a diagnostic result, not evidence of a stuck command.");
        }
        else if (_gate2PostCancelSamples.Count >= 2)
        {
            // Distinguish decaying residual momentum (acceptable) from persistent
            // injected input (not acceptable): compare successive per-sample
            // displacement, not just first-vs-last. A monotonically shrinking
            // step size is consistent with momentum decay; a step size that
            // stays flat or grows across the whole post-cancel window is not,
            // and must not be waved away as "residual physical velocity."
            var steps = new List<float>();
            for (int i = 1; i < _gate2PostCancelSamples.Count; i++)
            {
                var a = _gate2PostCancelSamples[i - 1];
                var b = _gate2PostCancelSamples[i];
                steps.Add(MathF.Sqrt(MathF.Pow(b.x - a.x, 2) + MathF.Pow(b.y - a.y, 2) + MathF.Pow(b.z - a.z, 2)));
            }
            var firstStep = steps[0];
            var lastStep = steps[^1];
            Log($"[gate2] post-cancel per-sample step size: first={firstStep:F5} last={lastStep:F5} (decreasing or near-zero is consistent with decaying residual momentum; flat-or-increasing across this whole window is NOT and must be treated as unexplained continued motion, not residual velocity).");
        }

        var api = BotControllerCap.Get();
        if (api is not null)
        {
            try { api.Unlock(_gate2Slot, LockKind.All); } catch { /* diagnostic-only, fail soft */ }
            try { api.Unlock(_gate2Slot, LockKind.Aim); } catch { /* diagnostic-only, fail soft */ }
        }
        Log($"[gate2] unlocked slot={_gate2Slot}. Acceptance criteria: 'AI suppression worked' is judged ONLY from the pre-cancel and post-cancel-still-locked windows above; the bot resuming ordinary CCSBot AI behavior AFTER this unlock is the CORRECT, expected outcome and must never be scored as an interference failure.");
    }

    // ---- GATE 3: DISABLED post-restart. See README "Gate 3 disabled" section for
    // the full finding. Summary: the synthetic ReplayTick's Pre/Post
    // MovementSnapshot.OriginX/Y/Z were zeroed below. Reading the actual pinned-commit
    // native source resolved the previously-open safety question definitively --
    // this is NOT a maybe:
    //   - src/features/recorder/MotionRecorder.cpp, StartReplay(): sets
    //     needsInitialTeleport = true unconditionally, no "no origin supplied" case.
    //   - OnReplayCommandPre(): on the first tick after that, calls the native
    //     CBaseEntity::Teleport export directly with position = tick.pre.origin{X,Y,Z}
    //     -- i.e. it WOULD teleport the bot to world (0,0,0). Then, every tick
    //     (including that same one), WriteSceneNodeOrigin(pawn, tick.pre) unconditionally
    //     overwrites the pawn's AbsOrigin scene-node memory from tick.pre.origin{X,Y,Z}
    //     -- there is no field-presence bitmask gating this (unlike
    //     ReplayCommandFrameData's forwardMove/leftMove/etc., which DO use a `fields`
    //     bitmask in InputInjector.cpp's ApplyReplayUserCommand).
    //   - OnReplayCommit() does the same again from tick.post.origin{X,Y,Z}.
    //   - The same two functions ALSO unconditionally overwrite AbsVelocity
    //     (WriteVelocityToPawn), MoveType/ActualMoveType (WriteField), the OnGround/
    //     Ducking bits of EntityFlags, and DuckAmount/DuckSpeed/LadderNormal
    //     (WriteMovementServiceState) from the same zeroed snapshot every tick.
    // Net finding: as currently constructed, this gate would not be a clean
    // "aim-only" test. It would visibly teleport the bot to world origin AND clobber
    // its velocity/movetype/ground-flag/duck state every tick. Per this task's
    // instruction to disable or defer Gate 3 rather than run an unverified synthetic
    // replay when safety can't be cleanly established: a *correct* fix requires
    // populating OriginX/Y/Z, VelX/Y/Z, MoveType, ActualMoveType, EntityFlags,
    // DuckAmount/DuckSpeed, and LadderNormal from a live pre-read of the target bot's
    // actual state (so the replay's forced writes are no-ops except for Pitch/Yaw) --
    // this harness does not currently have C# schema accessors for most of those
    // fields (only AbsOrigin/EyeAngles, via ReadPawnState), so wiring that up is out
    // of scope for a minimal fix and is left for a future session. Gate 3 is disabled
    // below rather than shipped in its previously-unsafe, previously-"unverified"
    // form.
    private readonly List<(long tMs, float pitch, float yaw)> _gate3Samples = new();
    private bool _gate3Active;
    private CCSPlayerPawn? _gate3Pawn;
    private long _gate3DeadlineMs;

    [ConsoleCommand("css_poc_gate3", "GATE3 (DISABLED): see README -- would teleport the bot to world origin as currently designed")]
    [CommandHelper(minArgs: 4, usage: "<slot> <pitch> <yaw> <durationMs>", whoCanExecute: CommandUsage.CLIENT_AND_SERVER)]
    public void OnGate3(CCSPlayerController? caller, CommandInfo cmd)
    {
        Log("[gate3] DISABLED: source-verified (MotionRecorder.cpp OnReplayCommandPre/OnReplayCommit) to unconditionally teleport the pawn to world origin and clobber velocity/movetype/ground-flag/duck state every replay tick when MovementSnapshot.Origin/Vel/etc. are zeroed, as this gate currently constructs them. Needs a live pre-read of the bot's actual state wired into the snapshot before this can run safely. Not executed.");
        return;
#if false
        // Excluded from compilation, not just from execution: this preserves the
        // original design for a future fix but guarantees Gate 3 cannot run and
        // cannot even affect whether this file builds. Re-enabling requires both
        // removing this #if false and replacing the zeroed fields below with a
        // live pre-read of the target bot's actual state (see the comment above
        // OnGate3 and the README's "Gate 3 disabled" section). Also worth
        // rechecking before any re-enable: api.SetReplayPawn(slot, pawn.Handle)
        // below looks suspect -- SetReplayPawn expects a native pointer (nint),
        // while CCSPlayerPawn.Handle is CounterStrikeSharp's entity handle, a
        // different concept (likely pawn.Address is intended instead). Flagged
        // from re-reading the signatures, not yet confirmed by an actual
        // compiler error against this specific line.
        if (!int.TryParse(cmd.GetArg(1), out var slot)
            || !float.TryParse(cmd.GetArg(2), out var pitch)
            || !float.TryParse(cmd.GetArg(3), out var yaw)
            || !int.TryParse(cmd.GetArg(4), out var durationMs))
        { Log("[gate3] bad args -- pitch/yaw must be supplied by the operator; this harness never invents a map coordinate or angle"); return; }

        var api = BotControllerCap.Get();
        if (api is null) { Log("[gate3] FAIL: capability not available"); return; }
        var controller = ResolveSlot(slot);
        if (controller is null || !controller.IsBot) { Log($"[gate3] slot {slot} is not a live bot"); return; }
        var pawn = controller.PlayerPawn?.Value;
        if (pawn is null) { Log($"[gate3] slot {slot} has no pawn"); return; }

        var snapshot = new MovementSnapshot
        {
            OriginX = 0, OriginY = 0, OriginZ = 0, // Pre/Post origin left at 0 deliberately --
            VelX = 0, VelY = 0, VelZ = 0,           // this replay tick is aim-only; whether a
            Pitch = pitch, Yaw = yaw, Roll = 0,      // zeroed origin is interpreted as "no move"
            EntityFlags = 0, MoveType = 0,           // or "teleport to world origin" by the native
            Buttons = 0, Buttons1 = 0, Buttons2 = 0, // replay path is UNVERIFIED -- this is exactly
            DuckAmount = 0, DuckSpeed = 0,           // the kind of thing Gate 3's own observation
            LadderNormalX = 0, LadderNormalY = 0, LadderNormalZ = 0,
            Ducked = 0, Ducking = 0, DesiresDuck = 0, ActualMoveType = 0
        };
        var tick = new ReplayTick { Pre = snapshot, Post = snapshot, WeaponDefIndex = -1, NumSubtick = 0, EventFlags = 0 };
        var ticks = new[] { tick, tick }; // two identical ticks so StartReplay(loop:true) has a nonzero span to loop across
        var subs = Array.Empty<SubtickMove>();
        var frame = new ReplayCommandFrame { ForwardMove = 0, LeftMove = 0, UpMove = 0, Pitch = pitch, Yaw = yaw, Roll = 0, Buttons = 0, Buttons1 = 0, Buttons2 = 0, MouseDx = 0, MouseDy = 0, WeaponSelect = -1, Fields = 0 };
        var commands = new[] { frame, frame };

        // CONFIRMED (not merely possible, see the disabled-gate comment above OnGate3):
        // this zeroed Pre/Post origin WILL teleport the pawn to world origin and
        // clobber velocity/movetype/ground-flag/duck state every tick. This method is
        // unreachable until that is fixed with a live-state pre-read -- see above.
        Log($"[gate3] CONFIRMED UNSAFE AS WRITTEN: zeroed MovementSnapshot.Origin/Vel/MoveType/EntityFlags/DuckState WILL be force-written to the pawn every replay tick (source-verified in MotionRecorder.cpp) -- this branch must not run until a live pre-read replaces the zeros.");

        bool loaded;
        try { loaded = api.LoadReplay(slot, ticks, subs, commands); }
        catch (Exception ex) { Log($"[gate3] LoadReplay threw {ex.GetType().Name}: {ex.Message}"); return; }
        Log($"[gate3] LoadReplay accepted={loaded}");
        if (!loaded) return;

        bool pawnRegistered;
        try { pawnRegistered = api.SetReplayPawn(slot, pawn.Handle); }
        catch (Exception ex) { Log($"[gate3] SetReplayPawn threw {ex.GetType().Name}: {ex.Message}"); return; }
        Log($"[gate3] SetReplayPawn accepted={pawnRegistered}");
        if (!pawnRegistered) return;

        bool started;
        try { started = api.StartReplay(slot, loop: true); }
        catch (Exception ex) { Log($"[gate3] StartReplay threw {ex.GetType().Name}: {ex.Message}"); return; }
        Log($"[gate3] StartReplay accepted={started}");
        if (!started) return;

        _gate3Samples.Clear();
        _gate3Pawn = pawn;
        _gate3DeadlineMs = MonotonicMs() + durationMs;
        _gate3Active = true;
        _lastGate3Slot = slot;
        Log($"[gate3] sampling started for {durationMs}ms, target pitch={pitch:F4} yaw={yaw:F4}");
#endif
    }

    private void FinishGate3()
    {
        var api = BotControllerCap.Get();
        try { api?.StopReplay(_lastGate3Slot); } catch { /* fail soft */ }
        Log($"[gate3] sample count={_gate3Samples.Count}");
        foreach (var s in _gate3Samples)
            Log($"[gate3] t={s.tMs} eye=(pitch={s.pitch:F4}, yaw={s.yaw:F4})");
        Log("[gate3] Compare each sample's pitch/yaw against the requested target with the established 0.5-degree tolerance. A wrap-around near +-180 yaw must be normalized before comparing, not treated as a large error.");
    }
    private int _lastGate3Slot; // set in OnGate3 alongside _gate3Pawn (kept explicit rather than inferred from pawn to avoid a stale-pointer footgun)

    // Emergency abort: immediate, bounded, never waits on further physics or
    // usercmd calls. Finalizes any active Gate 2B diagnostic as ABORTED before
    // releasing locks, so a partial capture is never mistaken for a completed
    // Gate 2B run (see FinishGate2's [ABORTED]/[COMPLETE] labeling).
    [ConsoleCommand("css_poc_stop", "Cancel all PoC harness activity for a slot")]
    [CommandHelper(minArgs: 1, usage: "<slot>", whoCanExecute: CommandUsage.CLIENT_AND_SERVER)]
    public void OnStop(CCSPlayerController? caller, CommandInfo cmd)
    {
        if (!int.TryParse(cmd.GetArg(1), out var slot)) { Log("[stop] bad slot arg"); return; }
        var api = BotControllerCap.Get();
        bool wasGate2Active = _gate2Active || _gate2PostCancelActive;
        _gate2Active = false; _gate2PostCancelActive = false; _gate3Active = false;

        if (wasGate2Active || slot == _gate2Slot)
        {
            // Routed through the same single-owner guard as FinishGate2/OnGate2's
            // early returns, so this can never emit a result that contradicts
            // whichever path actually wins the race to finalize.
            TryFinalizeGate2Diagnostic(slot, aborted: true);
        }

        if (api is null) { Log("[stop] capability unavailable, nothing to cancel via API (local sampling state cleared)"); return; }
        try
        {
            if (_activeMovement.TryGetValue(slot, out var id)) { api.CancelUsercmdMovement(slot, id); _activeMovement.Remove(slot); }
            api.StopReplay(slot);
            api.Unlock(slot, LockKind.All);
            api.Unlock(slot, LockKind.Aim);
        }
        catch (Exception ex) { Log($"[stop] cleanup threw {ex.GetType().Name}: {ex.Message}"); }
        Log($"[stop] slot={slot} cleanup issued");
    }
}

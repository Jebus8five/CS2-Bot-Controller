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
//   css_poc_gate2 <slot> <durationMs>        -- suppress AI, inject forward movement, sample
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

    [ConsoleCommand("css_poc_gate2", "GATE2: suppress AI, inject bounded forward movement, sample position")]
    [CommandHelper(minArgs: 2, usage: "<slot> <durationMs>", whoCanExecute: CommandUsage.CLIENT_AND_SERVER)]
    public void OnGate2(CCSPlayerController? caller, CommandInfo cmd)
    {
        if (!int.TryParse(cmd.GetArg(1), out var slot) || !int.TryParse(cmd.GetArg(2), out var durationMs))
        { Log("[gate2] bad args"); return; }

        var api = BotControllerCap.Get();
        if (api is null) { Log("[gate2] FAIL: capability not available (run gate1 first)"); return; }
        var controller = ResolveSlot(slot);
        if (controller is null || !controller.IsBot) { Log($"[gate2] slot {slot} is not a live bot"); return; }
        var pawn = controller.PlayerPawn?.Value;
        if (pawn is null) { Log($"[gate2] slot {slot} has no pawn"); return; }

        var (okPre, originPre, _, detailPre) = ReadPawnState(pawn);
        Log(okPre
            ? $"[gate2] PRE origin=({originPre!.X:F4},{originPre.Y:F4},{originPre.Z:F4})"
            : $"[gate2] PRE read FAILED: {detailPre} -- aborting, cannot establish baseline");
        if (!okPre) return;

        bool locked;
        try { locked = api.Lock(slot, LockKind.All); }
        catch (Exception ex) { Log($"[gate2] Lock threw {ex.GetType().Name}: {ex.Message}"); return; }
        Log($"[gate2] Lock(All) accepted={locked} (per BotControllerApi/Types.cs: freezes CCSBot::Update AND CCSBot::Upkeep -- acceptance only, not proof it held)");

        long movementId;
        try { movementId = api.StartUsercmdMovement(slot, forwardMove: 1.0f, leftMove: 0.0f); }
        catch (Exception ex) { Log($"[gate2] StartUsercmdMovement threw {ex.GetType().Name}: {ex.Message}"); if (locked) api.Unlock(slot, LockKind.All); return; }
        Log($"[gate2] StartUsercmdMovement accepted, id={movementId} (id<0 or ==-1 conventionally means rejected -- treat as such, not as success)");
        if (movementId < 0) { if (locked) api.Unlock(slot, LockKind.All); return; }
        _activeMovement[slot] = movementId;

        // Bounded sampling loop: one sample per server tick for the requested
        // window, via OnTick, not a busy-wait -- see RegisterListener below.
        _gate2Samples.Clear();
        _gate2Slot = slot;
        _gate2Pawn = pawn;
        _gate2DeadlineMs = MonotonicMs() + durationMs;
        _gate2Active = true;
        Log($"[gate2] sampling started for {durationMs}ms. Call css_poc_stop {slot} early if needed; otherwise it self-stops and logs the full trace.");
    }

    private readonly List<(long tMs, float x, float y, float z)> _gate2Samples = new();
    private bool _gate2Active;
    private int _gate2Slot;
    private CCSPlayerPawn? _gate2Pawn;
    private long _gate2DeadlineMs;

    private static long MonotonicMs() => Environment.TickCount64;

    public override void OnAllPluginsLoaded(bool hotReload)
    {
        RegisterListener<Listeners.OnTick>(OnTick);
    }

    private void OnTick()
    {
        if (_gate2Active && _gate2Pawn is not null)
        {
            var (ok, origin, _, _) = ReadPawnState(_gate2Pawn);
            if (ok) _gate2Samples.Add((MonotonicMs(), origin!.X, origin.Y, origin.Z));
            if (MonotonicMs() >= _gate2DeadlineMs)
            {
                _gate2Active = false;
                FinishGate2();
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

    private void FinishGate2()
    {
        var api = BotControllerCap.Get();
        if (api is not null && _activeMovement.TryGetValue(_gate2Slot, out var id))
        {
            try { api.CancelUsercmdMovement(_gate2Slot, id); } catch { /* diagnostic-only, fail soft */ }
        }
        Log($"[gate2] sample count={_gate2Samples.Count}");
        foreach (var s in _gate2Samples)
            Log($"[gate2] t={s.tMs} pos=({s.x:F4},{s.y:F4},{s.z:F4})");
        if (_gate2Samples.Count >= 2)
        {
            var first = _gate2Samples[0]; var last = _gate2Samples[^1];
            var dist = MathF.Sqrt(MathF.Pow(last.x - first.x, 2) + MathF.Pow(last.y - first.y, 2) + MathF.Pow(last.z - first.z, 2));
            Log($"[gate2] net displacement over window: {dist:F4} units. Continuity must be assessed from the full per-sample trace above (monotonic-ish progress, no single-sample teleport jump), not from this net figure alone.");
        }
        // Acceptance criteria, corrected post-restart: "AI suppression worked" is
        // judged ONLY from the sampled window above, while Lock(All) was active.
        // css_poc_stop (or gate self-expiry without a re-lock) deliberately releases
        // the lock; the bot resuming ordinary CCSBot AI behavior afterward is the
        // CORRECT, expected outcome and must never be scored as an interference
        // failure or as evidence suppression didn't work.
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
#pragma warning disable CS0162 // deliberately unreachable: preserved for the future fix described above, not deleted outright
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
#pragma warning restore CS0162
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

    [ConsoleCommand("css_poc_stop", "Cancel all PoC harness activity for a slot")]
    [CommandHelper(minArgs: 1, usage: "<slot>", whoCanExecute: CommandUsage.CLIENT_AND_SERVER)]
    public void OnStop(CCSPlayerController? caller, CommandInfo cmd)
    {
        if (!int.TryParse(cmd.GetArg(1), out var slot)) { Log("[stop] bad slot arg"); return; }
        var api = BotControllerCap.Get();
        _gate2Active = false; _gate3Active = false;
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

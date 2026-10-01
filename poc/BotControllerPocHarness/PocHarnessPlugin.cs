// BotController control-chain PoC harness -- DISPOSABLE, EXPERIMENTAL, NOT PRODUCTION.
//
// Scope, deliberately narrow per this task's authorization:
//   - Mostly uses BotController's existing public control surface
//     (IBotControllerApi) plus diagnostic-only native exports (Gate2B/Gate2C,
//     this file's own DllImports). One exception, explicitly authorized: the
//     css_poc_lockaim/css_poc_setaim commands below expose one new, minimal
//     native capability -- BotController_SetEyeAngles -- added to
//     BotController's own source (BotController.h/.cpp, exports.cpp) this
//     session. It installs no new hook; it exposes an existing, already-
//     installed engine-call primitive (previously replay-only, now generic
//     and reusable) under a new export. No Gate3/replay/synthetic-subtick
//     behavior was added, enabled, or changed -- see that export's own
//     comment in exports.cpp. (An earlier version of this harness also
//     P/Invoked four native "hook-call-counter" exports; those were found
//     not to exist in this build and were removed -- see the comment above
//     the remaining BotController_GetVersion DllImport.)
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
//   css_poc_gate2c <slot> <ms> <writeScale> [lockMode] [gate2cOnly] [forwardMove] [leftMove]
//                  [reverseAtMs] [waitForRestMs]
//                                             -- like gate2, but ALSO starts the Gate2C native
//                                                diagnostic, which additionally writes the same
//                                                movement intent directly into CMoveData inside
//                                                the ProcessMovement pre-hook. writeScale is
//                                                required and explicit -- there is no default;
//                                                see Gate2CDiagnostics.h for why 450 must not be
//                                                assumed and 1.0 is not asserted as established.
//                                                forwardMove/leftMove (default 1.0/0.0, each in
//                                                [-1,1]) select direction, including negative for
//                                                backward/lateral-reverse. reverseAtMs (disabled
//                                                by default), if set, flips both signs once via
//                                                the existing UpdateUsercmdMovement mid-run.
//                                                waitForRestMs (disabled by default), if set,
//                                                locks the slot and waits up to that long for
//                                                entity velocity to settle near zero before
//                                                starting capture at all -- see StartGate2CDiagnostic.
//   css_poc_gate3 <slot> <pitch> <yaw> <ms>  -- DISABLED, see comment above OnGate3 and README
//   css_poc_teleport <slot> <x> <y> <z> <pitch> <yaw> [roll]
//                                             -- teleports a bot to exact coordinates/orientation
//                                                with zero velocity, via CounterStrikeSharp's own
//                                                CBaseEntity.Teleport -- independent of Gate3/replay
//                                                and of BotController's own native Teleport export.
//                                                Isolated-server-only (checked once at Load(): the
//                                                ModuleDirectory must EXACTLY equal the isolated
//                                                server's canonical harness directory, no
//                                                junctions/symlinks -- fails closed); refuses while
//                                                any Gate2/Gate2C/Gate3 phase is active. Verifies
//                                                3 ticks later against a freshly re-resolved pawn.
//   css_poc_lockaim <slot>                   -- Lock(Aim) only for slot: suppresses the bot's own
//                                                Upkeep/UpdateLookAngles (its own aim decisions),
//                                                leaves Update (movement) completely untouched --
//                                                does NOT inject movement, unlike css_poc_gate2's
//                                                lockMode=aim. Unlock via css_poc_stop.
//                                                Isolated-server-only (same gate as css_poc_teleport).
//   css_poc_setaim <slot> <pitch> <yaw>      -- writes a live bot's eye angles via BotController's
//                                                native BotController_SetEyeAngles export (the same
//                                                generic engine-call primitive replay uses
//                                                internally, independent of Gate3/replay/synthetic-
//                                                subtick state). Does not lock anything itself --
//                                                call css_poc_lockaim first to suppress the bot's own
//                                                aim, or leave it unlocked to observe the two race.
//                                                pitch is bounded to [-89,89] (the universal Source-
//                                                engine look-up/down range, not a map-specific guess);
//                                                yaw is unconstrained (the native side wraps it).
//                                                Isolated-server-only; refused while a css_poc_teleport
//                                                verification is pending (same confound as Gate2/
//                                                Gate2C) but NOT while Gate2/Gate2C movement is active
//                                                -- aim and movement are independently controllable by
//                                                design. Samples EyeAngles for several ticks after the
//                                                write to show the angle holds, not just that it landed
//                                                once; a new css_poc_setaim call is refused while a
//                                                previous one's sampling window is still open.
//   css_poc_weaponstate <slot>               -- log current weapon identity + clip1 ammo, independent
//                                                read-only, no mutation.
//   css_poc_firetest <slot> [pressCount=3] [pressDurationMs=50] [gapMs=200]
//                     [lockMode=none|aim|all, default none] [weaponDefIndex=4 Glock-18]
//                                             -- injects pressCount discrete primary-attack presses via
//                                                the existing public IBotControllerApi.InjectUsercmd
//                                                (no new native code) and verifies via independently-
//                                                read clip1 ammo, never via InjectUsercmd's own return
//                                                value. Forces a known weapon first (SwitchBotWeapon)
//                                                for a deterministic test. lockMode defaults to none --
//                                                deliberately the LEAST invasive tier, not assumed to
//                                                need Lock(All) the way movement did; aim/all are
//                                                available to escalate if runtime evidence shows none
//                                                is insufficient. Also runs an idle-hold window with no
//                                                further injection afterward, to catch stuck/runaway
//                                                firing. Isolated-server-only; refused while a
//                                                css_poc_teleport verification is pending or a previous
//                                                css_poc_firetest is still running.
//   css_poc_jumptest <slot> [jumpCount=2] [pressDurationMs=50] [lockMode=none|aim|all]
//                                             -- injects the candidate IN_JUMP bit (1<<1, HYPOTHESIS --
//                                                not yet independently verified, unlike the movement/
//                                                attack bits) and verifies via independently-read
//                                                LastJumpTick/AbsVelocity.Z/origin.Z, never injection
//                                                acceptance. Waits for a confirmed/timed-out grounded
//                                                state before each discrete jump. Same none-first
//                                                lockMode tiering and isolated-only/teleport-pending
//                                                guard as css_poc_firetest.
//   css_poc_crouchtest <slot> [holdCount=2] [holdMs=800] [lockMode=none|aim|all]
//                                             -- injects/holds the candidate IN_DUCK bit (1<<2,
//                                                HYPOTHESIS) and verifies via independently-read
//                                                MovementServices.Ducked/Ducking/DuckAmount, including
//                                                a post-release check that it returns toward un-crouched.
//   css_poc_stop <slot>                     -- cancel all injections/locks/replay for slot, unlock

using System.Globalization;
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

    // ---- Gate 2C diagnostic-only exports (native, ProcessMovement-direct-write
    // experiment). Independent of the Gate 2B exports above -- separate native
    // state, separate buffers, separate finalize guard below. writeScale has no
    // default in the native layer (see Gate2CDiagnostics.h); this harness
    // requires it as an explicit console-command argument for the same reason.
    [DllImport("BotController", CallingConvention = CallingConvention.Cdecl)]
    private static extern int BotController_Gate2CDiagnosticStart(int slot, float writeScale);

    [DllImport("BotController", CallingConvention = CallingConvention.Cdecl)]
    private static extern int BotController_Gate2CDiagnosticMarkCancelled(int slot);

    [DllImport("BotController", CallingConvention = CallingConvention.Cdecl)]
    private static extern int BotController_Gate2CDiagnosticFinalize(int slot, int aborted);

    [DllImport("BotController", CallingConvention = CallingConvention.Cdecl)]
    private static extern long BotController_StartUsercmdMovementGate2COnly(int slot, float forwardMove, float leftMove, int maxDurationMs);

    // ---- Live aim-control export (native, generic -- not diagnostic-only
    // like the Gate2B/Gate2C exports above). Writes one bot's eye angles via
    // BotController::ApplyEyeAngles, the same primitive replay uses
    // internally; independent of Lock state and of Gate3/replay/synthetic-
    // subtick. Returns 0 on success, -1 on any failure (invalid slot, no
    // live bot for that slot, stale/missing pawn) -- see exports.cpp.
    [DllImport("BotController", CallingConvention = CallingConvention.Cdecl)]
    private static extern int BotController_SetEyeAngles(int slot, float pitch, float yaw);

    public override void Load(bool hotReload)
    {
        _logPath = Path.Combine(ModuleDirectory, "poc-harness.log");
        Log($"[load] PocHarnessPlugin loaded. hotReload={hotReload}");

        // css_poc_teleport gate: verified once here, at load time. This plugin's
        // own ModuleDirectory must EXACTLY equal the isolated server's
        // canonical harness directory (TeleportSafety.IsolatedHarnessModuleDirectory)
        // -- not a substring match, so backup copies such as
        // C:\CS2IsolatedBCTest-*-backup-* and any other install location
        // (production, V07) never qualify. See ConfirmIsolatedEnvironment for
        // the full list of checks. Any failure, including an exception,
        // leaves _isolatedEnvironmentConfirmed false -- fail closed.
        _isolatedEnvironmentConfirmed = false;
        string envDetail;
        try
        {
            _isolatedEnvironmentConfirmed = ConfirmIsolatedEnvironment(ModuleDirectory, out envDetail);
        }
        catch (Exception ex)
        {
            _isolatedEnvironmentConfirmed = false;
            envDetail = $"check threw {ex.GetType().Name}: {ex.Message}";
        }
        Log($"[load] isolated-environment check: ModuleDirectory={ModuleDirectory} confirmed={_isolatedEnvironmentConfirmed} ({envDetail})" +
            (_isolatedEnvironmentConfirmed ? "" : " -- failing closed (css_poc_teleport disabled)."));
    }

    private bool _isolatedEnvironmentConfirmed;

    public override void Unload(bool hotReload)
    {
        ClearTeleportVerification($"plugin unload (hotReload={hotReload})");
        ClearAimVerification($"plugin unload (hotReload={hotReload})");
        ClearFireTest($"plugin unload (hotReload={hotReload})");
        ClearJumpTest($"plugin unload (hotReload={hotReload})");
        ClearCrouchTest($"plugin unload (hotReload={hotReload})");
        base.Unload(hotReload);
    }

    // Canonical, exact isolated-root check. All of these must hold:
    //   1. running on Windows (the isolated server is a Windows install);
    //   2. the raw ModuleDirectory string is lexically the canonical path
    //      (TeleportSafety.IsCanonicalIsolatedModuleDirectory -- exact,
    //      case-insensitive, backslashes only, at most one trailing separator);
    //   3. Path.GetFullPath of it is ALSO exactly the canonical path (rules out
    //      relative segments, 8.3 short names, device/UNC prefixes);
    //   4. every directory from the harness directory up to the drive root
    //      exists and none is a reparse point (junction/symlink), so a link
    //      elsewhere pointing into, or out of, the isolated tree cannot pass.
    private static bool ConfirmIsolatedEnvironment(string? moduleDirectory, out string detail)
    {
        if (!OperatingSystem.IsWindows()) { detail = "not Windows"; return false; }
        if (!TeleportSafety.IsCanonicalIsolatedModuleDirectory(moduleDirectory))
        {
            detail = $"ModuleDirectory is not exactly {TeleportSafety.IsolatedHarnessModuleDirectory}";
            return false;
        }
        var full = Path.GetFullPath(moduleDirectory!);
        if (!TeleportSafety.IsCanonicalIsolatedModuleDirectory(full))
        {
            detail = $"GetFullPath resolved to non-canonical {full}";
            return false;
        }
        for (var dir = new DirectoryInfo(full); dir is not null; dir = dir.Parent)
        {
            if (!dir.Exists) { detail = $"{dir.FullName} does not exist"; return false; }
            if (dir.Parent is not null && (dir.Attributes & FileAttributes.ReparsePoint) != 0)
            {
                detail = $"{dir.FullName} is a reparse point (junction/symlink)";
                return false;
            }
        }
        detail = "exact canonical isolated harness directory, no reparse points";
        return true;
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

    // ---- Gate2C-only precision-control extension: independent entity-velocity
    // read, kept as a SEPARATE sibling function rather than folded into
    // ReadPawnState above so every existing gate2/gate2c/gate3/baseline call
    // site is completely untouched by this addition. `AbsVelocity` is a real,
    // callable CBaseEntity property (confirmed via a get_AbsVelocity getter
    // method present in the referenced CounterStrikeSharp.API.dll -- it has no
    // XML doc comment in this API version, which is why it doesn't show up in
    // a docs-only search, but the compiled getter is there), read the exact
    // same way AbsOrigin already is. This is deliberately INDEPENDENT of
    // Gate2C's own native CMoveData postVel readback -- the whole point is to
    // have two separate measurement paths to cross-check against each other,
    // not a replacement for either. NOT independently verified in this
    // project's own prior sessions the way CMoveData's offsets were: treat the
    // first live reading of this as needing a sanity cross-check against the
    // already-trusted native postVel for the same tick before relying on it
    // further.
    private static (bool ok, Vector? velocity, string detail) ReadPawnVelocity(CCSPlayerPawn pawn)
    {
        try
        {
            var velocity = pawn.AbsVelocity;
            if (velocity is null) return (false, null, "AbsVelocity null");
            return (true, velocity, "ok");
        }
        catch (Exception ex) { return (false, null, $"unreadable:{ex.GetType().Name}"); }
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

    // ---- Bot positioning for controlled movement tests. Uses only
    // CounterStrikeSharp's own public CBaseEntity.Teleport(Vector, QAngle,
    // Vector) -- confirmed present in the referenced API assembly, documented
    // as teleporting an entity to an explicit position/angles/velocity. Does
    // NOT touch BotController's own native Teleport export, MotionRecorder's
    // disabled Gate3/replay path, or any BotController state at all -- this
    // is a pure CounterStrikeSharp API call on a pawn resolved fresh from the
    // slot via ResolveLiveBotPawn.
    //
    // Delayed verification never holds on to a pawn object across ticks: only
    // the slot and the pawn's raw entity handle (index + serial) are stored,
    // and the pawn is re-resolved and re-validated from the slot at read time.
    // If the controller/pawn is gone, dead, or a different entity by then, the
    // read is skipped -- a stale native pointer is never dereferenced.
    // Pending state is cleared by: the verification itself, css_poc_stop,
    // map start/end, plugin unload, and a wall-clock expiry (in case OnTick
    // stops firing, e.g. hibernation) -- see ClearTeleportVerification. ----
    [ConsoleCommand("css_poc_teleport", "Teleport a bot to exact coordinates/orientation with zero velocity (diagnostic-only, isolated-server-only)")]
    [CommandHelper(minArgs: 6, usage: "<slot> <x> <y> <z> <pitch> <yaw> [roll, default 0]", whoCanExecute: CommandUsage.CLIENT_AND_SERVER)]
    public void OnTeleport(CCSPlayerController? caller, CommandInfo cmd)
    {
        if (!_isolatedEnvironmentConfirmed)
        {
            Log($"[teleport] REFUSED: isolated-server environment not confirmed (ModuleDirectory={ModuleDirectory}) -- failing closed.");
            return;
        }

        // Deliberately checked, never set, by this command -- teleport never
        // touches any gate's flags, so it cannot leave an admission guard stuck.
        var busy = TeleportSafety.TeleportBlockReason(
            _gate2Active, _gate2PostCancelActive, _gate3Active,
            _gate2cWaitingForRest, _gate2cActive, _gate2cPostCancelActive);
        if (busy is not null)
        {
            int busySlot = _gate2cWaitingForRest ? _gate2cWaitSlot
                : (_gate2cActive || _gate2cPostCancelActive) ? _gate2cSlot
                : (_gate2Active || _gate2PostCancelActive) ? _gate2Slot
                : _lastGate3Slot;
            Log($"[teleport] REJECTED: {busy} is active on slot {busySlot} -- call css_poc_stop {busySlot} first, or wait for it to finish.");
            return;
        }

        ReapExpiredTeleportVerification();
        if (_teleportVerifyPending)
        {
            Log($"[teleport] REJECTED: a previous teleport on slot {_teleportVerifySlot} is still being verified -- try again shortly (or css_poc_stop {_teleportVerifySlot}).");
            return;
        }

        var rawArgs = new string?[7];
        for (int i = 0; i < rawArgs.Length; i++) rawArgs[i] = i + 1 < cmd.ArgCount ? cmd.GetArg(i + 1) : null;
        if (!TeleportSafety.TryParseTeleportArgs(rawArgs, out var req, out var parseError))
        { Log($"[teleport] {parseError}"); return; }

        var (_, pawn, why) = ResolveLiveBotPawn(req.Slot);
        if (pawn is null) { Log($"[teleport] REFUSED: slot {req.Slot} {why}"); return; }

        uint pawnHandleRaw;
        try { pawnHandleRaw = pawn.EntityHandle.Raw; }
        catch (Exception ex) { Log($"[teleport] REFUSED: could not read pawn entity handle ({ex.GetType().Name}: {ex.Message})"); return; }

        var (okPre, originPre, eyePre, detailPre) = ReadPawnState(pawn);
        Log(okPre
            ? $"[teleport] PRE slot={req.Slot} origin=({originPre!.X:F4},{originPre.Y:F4},{originPre.Z:F4}) eye={FormatAngles(eyePre)}"
            : $"[teleport] PRE read FAILED: {detailPre} -- proceeding anyway, teleport does not depend on a successful pre-read");

        Log($"[teleport] requesting slot={req.Slot} pos=({req.X:F4},{req.Y:F4},{req.Z:F4}) " +
            $"angles=(pitch={req.Pitch:F4},yaw={req.Yaw:F4},roll={req.Roll:F4}) velocity=(0,0,0)");

        try
        {
            pawn.Teleport(new Vector(req.X, req.Y, req.Z), new QAngle(req.Pitch, req.Yaw, req.Roll), new Vector(0f, 0f, 0f));
        }
        catch (Exception ex)
        {
            Log($"[teleport] Teleport threw {ex.GetType().Name}: {ex.Message}");
            return;
        }

        // Verify a few ticks later, after the engine has actually processed
        // the teleport -- not synchronously in this same frame, since a
        // same-frame readback could still show pre-teleport state.
        _teleportVerifySlot = req.Slot;
        _teleportVerifyPawnHandleRaw = pawnHandleRaw;
        _teleportVerifyRequested = req;
        _teleportVerifyTicksRemaining = TeleportSafety.VerifyDelayTicks;
        _teleportVerifyExpiresAtMs = MonotonicMs() + TeleportSafety.VerifyExpiryMs;
        _teleportVerifyPending = true;
    }

    private bool _teleportVerifyPending;
    private int _teleportVerifySlot;
    private uint _teleportVerifyPawnHandleRaw;
    private TeleportRequest _teleportVerifyRequested;
    private int _teleportVerifyTicksRemaining;
    private long _teleportVerifyExpiresAtMs;

    // Resolves and validates a live bot pawn for a slot from scratch: valid
    // controller, is a bot, pawn alive, valid pawn handle, valid pawn entity.
    // Never throws; on any failure returns a null pawn and a reason.
    private static (CCSPlayerController? controller, CCSPlayerPawn? pawn, string why) ResolveLiveBotPawn(int slot)
    {
        try
        {
            var controller = ResolveSlot(slot);
            if (controller is null || !controller.IsValid) return (null, null, "has no valid controller");
            if (!controller.IsBot) return (controller, null, "is not a bot (bot-only)");
            if (!controller.PawnIsAlive) return (controller, null, "pawn is not alive");
            var handle = controller.PlayerPawn;
            if (handle is null || !handle.IsValid) return (controller, null, "pawn handle is invalid");
            var pawn = handle.Value;
            if (pawn is null || !pawn.IsValid) return (controller, null, "pawn entity is invalid");
            return (controller, pawn, "ok");
        }
        catch (Exception ex) { return (null, null, $"could not be resolved ({ex.GetType().Name}: {ex.Message})"); }
    }

    private void ClearTeleportVerification(string reason)
    {
        if (!_teleportVerifyPending) return;
        _teleportVerifyPending = false;
        _teleportVerifyPawnHandleRaw = 0;
        Log($"[teleport] pending verification for slot {_teleportVerifySlot} cleared without a POST read: {reason}");
    }

    private void ReapExpiredTeleportVerification()
    {
        if (_teleportVerifyPending && TeleportSafety.IsVerificationExpired(MonotonicMs(), _teleportVerifyExpiresAtMs))
            ClearTeleportVerification($"expired (no verification within {TeleportSafety.VerifyExpiryMs}ms -- OnTick not firing?)");
    }

    // Called from OnTick. Never throws: any failure is logged and the pending
    // state is already cleared before any native read is attempted.
    private void TickTeleportVerification()
    {
        if (!_teleportVerifyPending) return;
        ReapExpiredTeleportVerification();
        if (!_teleportVerifyPending) return;
        if (--_teleportVerifyTicksRemaining > 0) return;

        _teleportVerifyPending = false;
        var slot = _teleportVerifySlot;
        var req = _teleportVerifyRequested;
        try
        {
            var (_, pawn, why) = ResolveLiveBotPawn(slot);
            if (pawn is null) { Log($"[teleport] POST skipped: slot {slot} {why} -- not reading a stale pawn."); return; }
            if (pawn.EntityHandle.Raw != _teleportVerifyPawnHandleRaw)
            {
                Log($"[teleport] POST skipped: slot {slot} now has a different pawn entity " +
                    $"(handle {pawn.EntityHandle.Raw:X8} != teleported {_teleportVerifyPawnHandleRaw:X8}) -- not attributing it to this teleport.");
                return;
            }

            var (ok, origin, eye, detail) = ReadPawnState(pawn);
            if (!ok) { Log($"[teleport] POST read FAILED: {detail}"); return; }

            // Position: what was requested vs where the bot is N ticks later.
            // The bot is NOT locked, so its own AI/physics may have moved it
            // during those ticks -- the displacement is observation only, not
            // a measure of teleport error.
            var displacement = MathF.Sqrt(MathF.Pow(origin!.X - req.X, 2) + MathF.Pow(origin.Y - req.Y, 2) + MathF.Pow(origin.Z - req.Z, 2));
            Log($"[teleport] POST (after {TeleportSafety.VerifyDelayTicks} ticks) slot={slot} " +
                $"requestedPos=({req.X:F4},{req.Y:F4},{req.Z:F4}) observedPos=({origin.X:F4},{origin.Y:F4},{origin.Z:F4}) " +
                $"displacementSinceRequest={displacement:F4} (includes any bot movement during the interval -- NOT a teleport error measure)");

            // Angles: requested, then each observed source separately. For a
            // bot, EyeAngles and the entity's AbsRotation are distinct and may
            // legitimately diverge from the request once the AI resumes aiming.
            QAngle? absRot = null;
            try { absRot = pawn.AbsRotation; } catch { /* reported as unreadable below */ }
            Log($"[teleport] POST angles slot={slot} requested=(pitch={req.Pitch:F4},yaw={req.Yaw:F4},roll={req.Roll:F4}) " +
                $"observedEye={FormatAngles(eye)} eyeDelta={FormatAngleDelta(eye, req)} " +
                $"observedAbsRotation={FormatAngles(absRot)} absRotationDelta={FormatAngleDelta(absRot, req)} " +
                "(deltas wrapped to [-180,180); observational only)");

            var (velOk, velocity, velDetail) = ReadPawnVelocity(pawn);
            Log($"[teleport] POST velocity slot={slot} " +
                (velOk ? $"observed=({velocity!.X:F4},{velocity.Y:F4},{velocity.Z:F4}) |v|={VectorMagnitude(velocity):F4} (requested 0,0,0)"
                       : $"unreadable:{velDetail}"));
        }
        catch (Exception ex)
        {
            Log($"[teleport] POST verification threw {ex.GetType().Name}: {ex.Message}");
        }
    }

    private static string FormatAngles(QAngle? a) =>
        a is null ? "unreadable" : $"(pitch={a.X:F4},yaw={a.Y:F4},roll={a.Z:F4})";

    private static string FormatAngleDelta(QAngle? observed, TeleportRequest req) =>
        observed is null ? "n/a"
            : $"(pitch={TeleportSafety.WrapAngleDelta(observed.X, req.Pitch):F4},yaw={TeleportSafety.WrapAngleDelta(observed.Y, req.Yaw):F4},roll={TeleportSafety.WrapAngleDelta(observed.Z, req.Roll):F4})";

    // ---- AIM: Lock(Aim) only -- suppresses the bot's own aim decisions
    // (Upkeep/UpdateLookAngles) without touching movement (Update) at all
    // and without injecting any movement itself, unlike css_poc_gate2's
    // lockMode=aim (which always also calls StartUsercmdMovement). Unlocked
    // via the existing css_poc_stop, which already calls Unlock(..., Aim)
    // unconditionally -- no new cleanup path needed.
    [ConsoleCommand("css_poc_lockaim", "AIM: Lock(Aim) only for a slot -- suppress the bot's own aim, leave movement untouched (diagnostic-only, isolated-server-only)")]
    [CommandHelper(minArgs: 1, usage: "<slot>", whoCanExecute: CommandUsage.CLIENT_AND_SERVER)]
    public void OnLockAim(CCSPlayerController? caller, CommandInfo cmd)
    {
        if (!_isolatedEnvironmentConfirmed)
        {
            Log($"[lockaim] REFUSED: isolated-server environment not confirmed (ModuleDirectory={ModuleDirectory}) -- failing closed.");
            return;
        }
        if (!int.TryParse(cmd.GetArg(1), out var slot)) { Log("[lockaim] bad slot arg"); return; }

        var (_, pawn, why) = ResolveLiveBotPawn(slot);
        if (pawn is null) { Log($"[lockaim] REFUSED: slot {slot} {why}"); return; }

        var api = BotControllerCap.Get();
        if (api is null) { Log("[lockaim] FAIL: capability not available (run gate1 first)"); return; }

        bool locked;
        try { locked = api.Lock(slot, LockKind.Aim); }
        catch (Exception ex) { Log($"[lockaim] Lock threw {ex.GetType().Name}: {ex.Message}"); return; }
        Log($"[lockaim] Lock(Aim) accepted={locked} for slot {slot} -- suppresses only the bot's own Upkeep/UpdateLookAngles " +
            "(its own aim decisions); Update (movement) is completely untouched, so css_poc_gate2/gate2c remain independently " +
            $"usable on this slot while aim is locked. Unlock via css_poc_stop {slot}.");
    }

    // ---- AIM: css_poc_setaim -- writes a live bot's eye angles via
    // BotController_SetEyeAngles, the native export wrapping the SAME
    // generic engine-call primitive (BotController::ApplyEyeAngles) that
    // replay uses internally. Does not depend on Gate3/replay/synthetic-
    // subtick state, and does not itself lock anything -- css_poc_lockaim
    // is a separate, orthogonal step. Does not block on Gate2/Gate2C
    // movement being active (aim and movement are independently
    // controllable by design); it DOES block on a pending css_poc_teleport
    // verification, since that reads EyeAngles too and would be confounded
    // the same way Gate2/Gate2C already guard against. ----
    [ConsoleCommand("css_poc_setaim", "AIM: write a live bot's eye angles via BotController's native SetEyeAngles export (diagnostic-only, isolated-server-only)")]
    [CommandHelper(minArgs: 3, usage: "<slot> <pitch> <yaw>", whoCanExecute: CommandUsage.CLIENT_AND_SERVER)]
    public void OnSetAim(CCSPlayerController? caller, CommandInfo cmd)
    {
        if (!_isolatedEnvironmentConfirmed)
        {
            Log($"[setaim] REFUSED: isolated-server environment not confirmed (ModuleDirectory={ModuleDirectory}) -- failing closed.");
            return;
        }

        // Shared concern with Gate2/Gate2C: a pending css_poc_teleport
        // verification reads EyeAngles a few ticks after the teleport, so
        // writing a new angle inside that window would confound both
        // results. Deliberately does NOT check Gate2/Gate2C/Gate3 state --
        // aim must remain controllable while movement is active (criterion
        // F of this capability's acceptance tests).
        ReapExpiredTeleportVerification();
        if (_teleportVerifyPending)
        {
            Log($"[setaim] REJECTED: a css_poc_teleport verification is still pending on slot {_teleportVerifySlot} -- " +
                $"retry in a moment (or css_poc_stop {_teleportVerifySlot}).");
            return;
        }

        if (_aimActive)
        {
            Log($"[setaim] REJECTED: a previous css_poc_setaim sampling window is still open on slot {_aimSlot} -- " +
                $"wait for it to finish, or css_poc_stop {_aimSlot}.");
            return;
        }

        var rawArgs = new string?[3];
        for (int i = 0; i < rawArgs.Length; i++) rawArgs[i] = i + 1 < cmd.ArgCount ? cmd.GetArg(i + 1) : null;
        if (!AimSafety.TryParseSetAimArgs(rawArgs, out var req, out var parseError))
        { Log($"[setaim] {parseError}"); return; }

        var (_, pawn, why) = ResolveLiveBotPawn(req.Slot);
        if (pawn is null) { Log($"[setaim] REFUSED: slot {req.Slot} {why}"); return; }

        uint pawnHandleRaw;
        try { pawnHandleRaw = pawn.EntityHandle.Raw; }
        catch (Exception ex) { Log($"[setaim] REFUSED: could not read pawn entity handle ({ex.GetType().Name}: {ex.Message})"); return; }

        var (okPre, _, eyePre, detailPre) = ReadPawnState(pawn);
        Log(okPre
            ? $"[setaim] PRE slot={req.Slot} observedEye={FormatEye(eyePre)}"
            : $"[setaim] PRE read FAILED: {detailPre} -- proceeding anyway, the write does not depend on a successful pre-read");

        Log($"[setaim] commanding slot={req.Slot} pitch={req.Pitch:F4} yaw={req.Yaw:F4} via BotController_SetEyeAngles");

        int writeStatus;
        try { writeStatus = BotController_SetEyeAngles(req.Slot, req.Pitch, req.Yaw); }
        catch (Exception ex)
        {
            Log($"[setaim] BotController_SetEyeAngles threw {ex.GetType().Name}: {ex.Message} -- treat as FAIL, no sampling window started.");
            return;
        }
        Log($"[setaim] engine write result status={writeStatus} (0=success, -1=failed closed -- invalid slot, no live bot, or stale/missing pawn; " +
            "this is the ENGINE CALL's own result, not independently-observed EyeAngles -- see the sampled trace below for that).");
        if (writeStatus != 0) { Log("[setaim] write FAILED -- no sampling window started."); return; }

        _aimSamples.Clear();
        _aimSlot = req.Slot;
        _aimPawnHandleRaw = pawnHandleRaw;
        _aimRequested = req;
        _aimTicksRemaining = AimVerifyTicks;
        _aimActive = true;
        Log($"[setaim] sampling independently-observed EyeAngles for {AimVerifyTicks} ticks to confirm the angle holds (not just that it landed once). " +
            $"Call css_poc_stop {req.Slot} early if needed.");
    }

    private const int AimVerifyTicks = 12;
    private const float AimAngleToleranceDeg = 1.0f;

    private bool _aimActive;
    private int _aimSlot;
    private uint _aimPawnHandleRaw;
    private AimRequest _aimRequested;
    private int _aimTicksRemaining;
    private readonly List<(long tMs, float pitch, float yaw)> _aimSamples = new();

    private static string FormatEye(QAngle? eye) =>
        eye is null ? "unreadable" : $"(pitch={eye.X:F4},yaw={eye.Y:F4})";

    // Called from OnTick while _aimActive. Never throws: any failure clears
    // the window and logs it rather than leaving it stuck open. Mirrors
    // TickTeleportVerification's re-resolve-and-validate shape exactly.
    private void TickAimVerification()
    {
        if (!_aimActive) return;

        try
        {
            var (_, pawn, why) = ResolveLiveBotPawn(_aimSlot);
            if (pawn is null) { FinishAim(aborted: true, $"pawn no longer resolvable ({why})"); return; }
            if (pawn.EntityHandle.Raw != _aimPawnHandleRaw)
            { FinishAim(aborted: true, "pawn entity changed mid-window (not attributing samples to this command)"); return; }

            var (ok, _, eye, _) = ReadPawnState(pawn);
            if (ok) _aimSamples.Add((MonotonicMs(), eye!.X, eye.Y));
        }
        catch (Exception ex) { FinishAim(aborted: true, $"sampling threw {ex.GetType().Name}: {ex.Message}"); return; }

        if (--_aimTicksRemaining <= 0) FinishAim(aborted: false, null);
    }

    // Logs the full sampled trace and a stability verdict, then clears the
    // window. aborted=true (pawn gone/changed mid-window) is logged as such
    // rather than silently producing a possibly-misleading verdict.
    private void FinishAim(bool aborted, string? abortReason)
    {
        _aimActive = false;
        var req = _aimRequested;
        var slot = _aimSlot;

        Log($"[setaim] sample count={_aimSamples.Count} requested=(pitch={req.Pitch:F4},yaw={req.Yaw:F4})" +
            (aborted ? $" -- ABORTED: {abortReason} (verdict below is based on whatever samples were collected before this)" : ""));
        foreach (var s in _aimSamples)
            Log($"[setaim] t={s.tMs} observedEye=(pitch={s.pitch:F4},yaw={s.yaw:F4}) " +
                $"delta=(pitch={TeleportSafety.WrapAngleDelta(s.pitch, req.Pitch):F4},yaw={TeleportSafety.WrapAngleDelta(s.yaw, req.Yaw):F4})");

        if (_aimSamples.Count == 0)
        {
            Log("[setaim] no samples collected -- cannot verify stability (treat as FAIL, not as a passive non-result).");
            return;
        }

        // Stability: the LAST sample must be within tolerance (reached the
        // target by the end of the window), AND every sample from the first
        // in-tolerance one onward must also stay in tolerance -- a target
        // that is reached and then drifts back off is not "stable", even if
        // the very last sample happens to be close again.
        var last = _aimSamples[^1];
        var lastPitchOk = MathF.Abs(TeleportSafety.WrapAngleDelta(last.pitch, req.Pitch)) <= AimAngleToleranceDeg;
        var lastYawOk = MathF.Abs(TeleportSafety.WrapAngleDelta(last.yaw, req.Yaw)) <= AimAngleToleranceDeg;

        int firstInToleranceIndex = -1;
        for (int i = 0; i < _aimSamples.Count; i++)
        {
            var pOk = MathF.Abs(TeleportSafety.WrapAngleDelta(_aimSamples[i].pitch, req.Pitch)) <= AimAngleToleranceDeg;
            var yOk = MathF.Abs(TeleportSafety.WrapAngleDelta(_aimSamples[i].yaw, req.Yaw)) <= AimAngleToleranceDeg;
            if (pOk && yOk) { firstInToleranceIndex = i; break; }
        }

        bool heldOnceReached = firstInToleranceIndex >= 0;
        if (heldOnceReached)
        {
            for (int i = firstInToleranceIndex; i < _aimSamples.Count; i++)
            {
                var pOk = MathF.Abs(TeleportSafety.WrapAngleDelta(_aimSamples[i].pitch, req.Pitch)) <= AimAngleToleranceDeg;
                var yOk = MathF.Abs(TeleportSafety.WrapAngleDelta(_aimSamples[i].yaw, req.Yaw)) <= AimAngleToleranceDeg;
                if (!pOk || !yOk) { heldOnceReached = false; break; }
            }
        }

        string verdict = !lastPitchOk || !lastYawOk
            ? "FAIL (final sample not within tolerance of requested angle)"
            : !heldOnceReached
                ? "FAIL (reached tolerance at some point but drifted back out before the window ended -- not stable)"
                : "PASS (reached the requested angle and held it, within " + AimAngleToleranceDeg + " deg, for the rest of the sampling window)";
        Log($"[setaim] slot={slot} STABILITY VERDICT: {verdict}");
    }

    private void ClearAimVerification(string reason)
    {
        if (!_aimActive) return;
        _aimActive = false;
        Log($"[setaim] sampling window for slot {_aimSlot} cleared without finishing: {reason}");
    }

    // ---- FIRE: css_poc_weaponstate / css_poc_firetest -- uses ONLY the
    // existing public IBotControllerApi.InjectUsercmd (already a production
    // method, not diagnostic-only), no native changes. Evidence comes from
    // independently-read CBasePlayerWeapon.Clip1 via CounterStrikeSharp's own
    // schema, never from InjectUsercmd's own return value (acceptance of an
    // injection is NOT proof a shot fired). ----

    private static (bool ok, string weaponName, int clip1, string detail) ReadWeaponState(CCSPlayerPawn pawn)
    {
        try
        {
            var weapon = pawn.WeaponServices?.ActiveWeapon.Value;
            if (weapon is null || !weapon.IsValid) return (false, "", 0, "no active weapon");
            return (true, weapon.DesignerName, weapon.Clip1, "ok");
        }
        catch (Exception ex) { return (false, "", 0, $"unreadable:{ex.GetType().Name}"); }
    }

    [ConsoleCommand("css_poc_weaponstate", "Log current weapon identity + clip1 ammo for slot (independent read-only, no mutation)")]
    [CommandHelper(minArgs: 1, usage: "<slot>", whoCanExecute: CommandUsage.CLIENT_AND_SERVER)]
    public void OnWeaponState(CCSPlayerController? caller, CommandInfo cmd)
    {
        if (!int.TryParse(cmd.GetArg(1), out var slot)) { Log("[weaponstate] bad slot arg"); return; }
        var controller = ResolveSlot(slot);
        if (controller is null || !controller.IsBot) { Log($"[weaponstate] slot {slot} is not a live bot -- refusing"); return; }
        var pawn = controller.PlayerPawn?.Value;
        if (pawn is null) { Log($"[weaponstate] slot {slot} has no pawn"); return; }
        var (ok, weapon, clip1, detail) = ReadWeaponState(pawn);
        Log(ok ? $"[weaponstate] slot={slot} weapon={weapon} clip1={clip1}" : $"[weaponstate] slot={slot} read FAILED: {detail}");
    }

    // Deliberately tiered, operator-controlled lockMode: "none" is the
    // DEFAULT (least invasive first), per this task's explicit instruction
    // not to assume Lock(All) is required for fire the way it was found
    // necessary for movement. This command makes the tier an explicit,
    // logged choice rather than hardcoding one -- the actual minimum
    // requirement is an empirical runtime question, answered by running
    // this command at each tier and reading the FIRE VERDICT below, not by
    // this code's own design.
    [ConsoleCommand("css_poc_firetest", "FIRE: inject discrete primary-attack presses and verify via independently-read clip1 ammo (diagnostic-only, isolated-server-only)")]
    [CommandHelper(minArgs: 1, usage: "<slot> [pressCount=3] [pressDurationMs=50] [gapMs=200] [lockMode=none|aim|all, default none] [weaponDefIndex=4 Glock-18]", whoCanExecute: CommandUsage.CLIENT_AND_SERVER)]
    public void OnFireTest(CCSPlayerController? caller, CommandInfo cmd)
    {
        if (!_isolatedEnvironmentConfirmed)
        {
            Log($"[firetest] REFUSED: isolated-server environment not confirmed (ModuleDirectory={ModuleDirectory}) -- failing closed.");
            return;
        }

        ReapExpiredTeleportVerification();
        if (_teleportVerifyPending)
        {
            Log($"[firetest] REJECTED: a css_poc_teleport verification is still pending on slot {_teleportVerifySlot} -- " +
                $"retry in a moment (or css_poc_stop {_teleportVerifySlot}).");
            return;
        }

        if (_fireActive)
        {
            Log($"[firetest] REJECTED: a previous css_poc_firetest is still running on slot {_fireSlot} -- " +
                $"wait for it to finish, or css_poc_stop {_fireSlot}.");
            return;
        }

        var rawArgs = new string?[6];
        for (int i = 0; i < rawArgs.Length; i++) rawArgs[i] = i + 1 < cmd.ArgCount ? cmd.GetArg(i + 1) : null;
        if (!FireSafety.TryParseFireTestArgs(rawArgs, out var req, out var parseError))
        { Log($"[firetest] {parseError}"); return; }

        var (_, pawn, why) = ResolveLiveBotPawn(req.Slot);
        if (pawn is null) { Log($"[firetest] REFUSED: slot {req.Slot} {why}"); return; }

        var api = BotControllerCap.Get();
        if (api is null) { Log("[firetest] FAIL: capability not available (run gate1 first)"); return; }

        // Deterministic test setup (per this task's instruction): force a
        // known, simple semi-auto weapon via the EXISTING public
        // SwitchBotWeapon, rather than trusting whatever the bot's own AI
        // happened to equip -- an existing, already-public mechanism, not
        // new architecture.
        bool switched;
        try { switched = api.SwitchBotWeapon(req.Slot, req.WeaponDefIndex); }
        catch (Exception ex) { Log($"[firetest] SwitchBotWeapon threw {ex.GetType().Name}: {ex.Message}"); return; }
        Log($"[firetest] SwitchBotWeapon(defIndex={req.WeaponDefIndex}) accepted={switched}");

        LockKind? lockKind = req.LockMode switch { FireLockMode.Aim => LockKind.Aim, FireLockMode.All => LockKind.All, _ => null };
        if (lockKind.HasValue)
        {
            bool locked;
            try { locked = api.Lock(req.Slot, lockKind.Value); }
            catch (Exception ex) { Log($"[firetest] Lock threw {ex.GetType().Name}: {ex.Message}"); return; }
            Log($"[firetest] Lock({lockKind.Value}) accepted={locked} (requested suppression tier for this test)");
        }
        else
        {
            Log("[firetest] lockMode=none -- no Lock call issued; testing whether injected fire works with ZERO suppression.");
        }

        var (okBefore, weaponBefore, clipBefore, detailBefore) = ReadWeaponState(pawn);
        if (!okBefore)
        {
            Log($"[firetest] REFUSED: cannot independently establish starting weapon/clip state ({detailBefore}) -- " +
                "required before injecting anything, aborting.");
            return;
        }
        Log($"[firetest] PRE slot={req.Slot} weapon={weaponBefore} clip1={clipBefore}");

        uint pawnHandleRaw;
        try { pawnHandleRaw = pawn.EntityHandle.Raw; }
        catch (Exception ex) { Log($"[firetest] REFUSED: could not read pawn entity handle ({ex.GetType().Name}: {ex.Message})"); return; }

        _fireSlot = req.Slot;
        _firePawnHandleRaw = pawnHandleRaw;
        _fireLockKind = lockKind;
        _firePressesRequested = req.PressCount;
        _firePressesIssued = 0;
        _firePressDurationMs = req.PressDurationMs;
        _fireGapMs = req.GapMs;
        _fireClipBefore = clipBefore;
        _fireWeaponBefore = weaponBefore;
        _fireInjectionIds.Clear();
        _fireTrace.Clear();
        _fireTrace.Add((MonotonicMs(), "PRE", clipBefore));
        _firePhase = FirePhase.Pressing;
        _fireNextPressAtMs = MonotonicMs();
        _fireActive = true;

        Log($"[firetest] starting {req.PressCount} discrete press(es), {req.PressDurationMs}ms hold each, {req.GapMs}ms gap, " +
            $"lockMode={(lockKind.HasValue ? lockKind.Value.ToString() : "none")}, buttonMask=IN_ATTACK(bit0) only. " +
            "Each press is independently injected and independently read back -- not a single sustained hold.");
    }

    private enum FirePhase { Pressing, Settling, IdleHold }

    private const long FireSettleMs = 300;    // after the last press, before reading the "after-shots" clip1
    private const long FireIdleHoldMs = 1000; // extra idle window, NO injection issued, to catch stuck/runaway firing (criterion D)

    private bool _fireActive;
    private int _fireSlot;
    private uint _firePawnHandleRaw;
    private LockKind? _fireLockKind;
    private int _firePressesRequested;
    private int _firePressesIssued;
    private int _firePressDurationMs;
    private int _fireGapMs;
    private long _fireNextPressAtMs;
    private long _fireSettleUntilMs;
    private long _fireIdleCheckUntilMs;
    private int _fireClipBefore;
    private string _fireWeaponBefore = "";
    private int _fireClipAfterShots;
    private FirePhase _firePhase;
    private readonly List<long> _fireInjectionIds = new();
    private readonly List<(long tMs, string note, int clip1)> _fireTrace = new();

    // Called from OnTick while _fireActive. Never throws: any failure clears
    // the test and logs it. Mirrors TickAimVerification's re-resolve-and-
    // validate shape.
    private void TickFireTest()
    {
        if (!_fireActive) return;

        CCSPlayerPawn pawn;
        try
        {
            var (_, p, why) = ResolveLiveBotPawn(_fireSlot);
            if (p is null) { FinishFireTest(aborted: true, $"pawn no longer resolvable ({why})"); return; }
            if (p.EntityHandle.Raw != _firePawnHandleRaw) { FinishFireTest(aborted: true, "pawn entity changed mid-test"); return; }
            pawn = p;
        }
        catch (Exception ex) { FinishFireTest(aborted: true, $"pawn re-resolve threw {ex.GetType().Name}: {ex.Message}"); return; }

        var api = BotControllerCap.Get();
        if (api is null) { FinishFireTest(aborted: true, "capability unavailable mid-test"); return; }

        long now = MonotonicMs();
        switch (_firePhase)
        {
            case FirePhase.Pressing:
                if (now < _fireNextPressAtMs) break;
                if (_firePressesIssued >= _firePressesRequested)
                {
                    _firePhase = FirePhase.Settling;
                    _fireSettleUntilMs = now + FireSettleMs;
                    break;
                }
                long injId;
                try { injId = api.InjectUsercmd(_fireSlot, FireSafety.AttackButtonMask, _firePressDurationMs); }
                catch (Exception ex)
                {
                    Log($"[firetest] InjectUsercmd threw {ex.GetType().Name}: {ex.Message} on press #{_firePressesIssued + 1}");
                    injId = -1;
                }
                _firePressesIssued++;
                _fireInjectionIds.Add(injId);
                Log($"[firetest] press #{_firePressesIssued}/{_firePressesRequested}: InjectUsercmd(buttonMask=IN_ATTACK, durationMs={_firePressDurationMs}) " +
                    $"returned id={injId} " + (injId < 0
                        ? "(REJECTED by the native layer -- see BotController_InjectUsercmd's own fail-closed conditions)"
                        : "(injection ACCEPTED -- lifecycle acceptance only, NOT proof a shot fired; see clip1 below for that)"));
                {
                    var (ok, _, clip1, detail) = ReadWeaponState(pawn);
                    _fireTrace.Add((now, ok ? $"after press #{_firePressesIssued}" : $"after press #{_firePressesIssued} READ FAILED: {detail}",
                        ok ? clip1 : _fireTrace.Count > 0 ? _fireTrace[^1].clip1 : _fireClipBefore));
                }
                _fireNextPressAtMs = now + _firePressDurationMs + _fireGapMs;
                break;

            case FirePhase.Settling:
                if (now < _fireSettleUntilMs) break;
                {
                    var (ok, _, clip1, detail) = ReadWeaponState(pawn);
                    _fireClipAfterShots = ok ? clip1 : _fireTrace[^1].clip1;
                    _fireTrace.Add((now, ok ? "after-shots settle" : $"after-shots settle READ FAILED: {detail}", _fireClipAfterShots));
                }
                _firePhase = FirePhase.IdleHold;
                _fireIdleCheckUntilMs = now + FireIdleHoldMs;
                break;

            case FirePhase.IdleHold:
                if (now < _fireIdleCheckUntilMs) break;
                {
                    var (ok, _, clip1, detail) = ReadWeaponState(pawn);
                    _fireTrace.Add((now, ok ? "idle-hold end (no further injection issued)" : $"idle-hold end READ FAILED: {detail}",
                        ok ? clip1 : _fireClipAfterShots));
                }
                FinishFireTest(aborted: false, null);
                break;
        }
    }

    // Logs the full trace, the FIRE verdict (clip1 before vs after-shots --
    // never injection acceptance alone), and the release-semantics verdict
    // (criterion D: no further clip1 drop during the idle-hold window),
    // then cancels any outstanding injections defensively before clearing.
    private void FinishFireTest(bool aborted, string? abortReason)
    {
        _fireActive = false;
        var slot = _fireSlot;

        Log($"[firetest] slot={slot} weapon={_fireWeaponBefore} requested presses={_firePressesRequested} issued={_firePressesIssued} " +
            $"lockMode={(_fireLockKind.HasValue ? _fireLockKind.Value.ToString() : "none")}" +
            (aborted ? $" -- ABORTED: {abortReason}" : ""));
        foreach (var t in _fireTrace)
            Log($"[firetest] t={t.tMs} {t.note} clip1={t.clip1}");

        if (!aborted && _fireTrace.Count > 0)
        {
            int shotsConsumed = _fireClipBefore - _fireClipAfterShots;
            Log($"[firetest] clip1 before={_fireClipBefore} after-shots={_fireClipAfterShots} consumed={shotsConsumed} " +
                "(INDEPENDENTLY OBSERVED engine ammo state via CounterStrikeSharp's own weapon schema, not an injection return value)");

            string fireVerdict = shotsConsumed <= 0
                ? "FAIL (clip1 did not decrease -- no evidence a shot was actually fired, regardless of injection acceptance)"
                : shotsConsumed >= _firePressesRequested
                    ? $"PASS (clip1 decreased by {shotsConsumed}, consistent with {_firePressesRequested} discrete shot(s) fired)"
                    : $"PARTIAL (clip1 decreased by {shotsConsumed}, fewer than the {_firePressesRequested} requested presses -- " +
                      "some presses may not have each registered as a separate shot; inspect the per-press trace above)";
            Log($"[firetest] FIRE VERDICT: {fireVerdict}");

            var idleEntry = _fireTrace[^1];
            if (idleEntry.note.StartsWith("idle-hold end", StringComparison.Ordinal))
            {
                int idleDrop = _fireClipAfterShots - idleEntry.clip1;
                Log(idleDrop > 0
                    ? $"[firetest] RELEASE-SEMANTICS VERDICT: FAIL -- clip1 dropped a further {idleDrop} during the idle-hold window with no injection active (firing appears stuck)."
                    : "[firetest] RELEASE-SEMANTICS VERDICT: PASS -- no further clip1 change during the idle-hold window with no injection active (not stuck).");
            }
        }

        var api = BotControllerCap.Get();
        foreach (var id in _fireInjectionIds)
        {
            if (id < 0) continue;
            try { api?.CancelUsercmdInjection(slot, id); } catch { /* best-effort, diagnostic only */ }
        }
    }

    private void ClearFireTest(string reason)
    {
        if (!_fireActive) return;
        _fireActive = false;
        var api = BotControllerCap.Get();
        foreach (var id in _fireInjectionIds)
        {
            if (id < 0) continue;
            try { api?.CancelUsercmdInjection(_fireSlot, id); } catch { /* best-effort */ }
        }
        Log($"[firetest] cleared without finishing for slot {_fireSlot}: {reason}");
    }

    // ---- JUMP: css_poc_jumptest -- same architecture as css_poc_firetest:
    // existing public InjectUsercmd only, no native changes. IN_JUMP=1<<1 is
    // a HYPOTHESIS (standard Source layout), not yet independently verified
    // in this codebase the way the movement/attack bits are -- a successful
    // jump arc (LastJumpTick advancing, vertical velocity/height) is itself
    // the verification, exactly as fire's clip1 decrease verified bit 0.
    private static (bool ok, bool onGround, int lastJumpTick, string detail) ReadJumpState(CCSPlayerPawn pawn)
    {
        try
        {
            bool onGround = pawn.OnGroundLastTick;
            var ms = pawn.MovementServices;
            if (ms is null) return (false, false, -1, "no MovementServices");
            return (true, onGround, ms.LastJumpTick, "ok");
        }
        catch (Exception ex) { return (false, false, -1, $"unreadable:{ex.GetType().Name}"); }
    }

    [ConsoleCommand("css_poc_jumptest", "JUMP: inject candidate IN_JUMP bit and verify via independently-observed LastJumpTick/velocity.Z/height (diagnostic-only, isolated-server-only)")]
    [CommandHelper(minArgs: 1, usage: "<slot> [jumpCount=2] [pressDurationMs=50] [lockMode=none|aim|all, default none]", whoCanExecute: CommandUsage.CLIENT_AND_SERVER)]
    public void OnJumpTest(CCSPlayerController? caller, CommandInfo cmd)
    {
        if (!_isolatedEnvironmentConfirmed)
        {
            Log($"[jumptest] REFUSED: isolated-server environment not confirmed (ModuleDirectory={ModuleDirectory}) -- failing closed.");
            return;
        }
        ReapExpiredTeleportVerification();
        if (_teleportVerifyPending)
        {
            Log($"[jumptest] REJECTED: a css_poc_teleport verification is still pending on slot {_teleportVerifySlot} -- retry shortly.");
            return;
        }
        if (_jumpActive)
        {
            Log($"[jumptest] REJECTED: a previous css_poc_jumptest is still running on slot {_jumpSlot} -- wait, or css_poc_stop {_jumpSlot}.");
            return;
        }

        var rawArgs = new string?[4];
        for (int i = 0; i < rawArgs.Length; i++) rawArgs[i] = i + 1 < cmd.ArgCount ? cmd.GetArg(i + 1) : null;
        if (!MotorTestSafety.TryParseJumpTestArgs(rawArgs, out var req, out var parseError))
        { Log($"[jumptest] {parseError}"); return; }

        var (_, pawn, why) = ResolveLiveBotPawn(req.Slot);
        if (pawn is null) { Log($"[jumptest] REFUSED: slot {req.Slot} {why}"); return; }
        var api = BotControllerCap.Get();
        if (api is null) { Log("[jumptest] FAIL: capability not available (run gate1 first)"); return; }

        LockKind? lockKind = req.LockMode switch { FireLockMode.Aim => LockKind.Aim, FireLockMode.All => LockKind.All, _ => null };
        if (lockKind.HasValue)
        {
            bool locked;
            try { locked = api.Lock(req.Slot, lockKind.Value); }
            catch (Exception ex) { Log($"[jumptest] Lock threw {ex.GetType().Name}: {ex.Message}"); return; }
            Log($"[jumptest] Lock({lockKind.Value}) accepted={locked} (requested suppression tier for this test)");
        }
        else
        {
            Log("[jumptest] lockMode=none -- no Lock call issued; testing whether injected jump works with ZERO suppression.");
        }

        var (okPre, onGroundPre, tickPre, detailPre) = ReadJumpState(pawn);
        var (okPosPre, originPre, _, detailPosPre) = ReadPawnState(pawn);
        if (!okPre || !okPosPre)
        {
            Log($"[jumptest] REFUSED: cannot independently establish starting ground/position state (jump:{detailPre} pos:{detailPosPre}) -- aborting.");
            return;
        }
        Log($"[jumptest] PRE slot={req.Slot} onGround={onGroundPre} lastJumpTick={tickPre} z={originPre!.Z:F4}");

        uint pawnHandleRaw;
        try { pawnHandleRaw = pawn.EntityHandle.Raw; }
        catch (Exception ex) { Log($"[jumptest] REFUSED: could not read pawn entity handle ({ex.GetType().Name}: {ex.Message})"); return; }

        _jumpSlot = req.Slot;
        _jumpPawnHandleRaw = pawnHandleRaw;
        _jumpLockKind = lockKind;
        _jumpCountRequested = req.JumpCount;
        _jumpAttemptsDone = 0;
        _jumpPressDurationMs = req.PressDurationMs;
        _jumpTrace.Clear();
        _jumpInjectionIds.Clear();
        _jumpPhase = JumpPhase.WaitGroundedBefore;
        _jumpPhaseDeadlineMs = MonotonicMs() + JumpGroundWaitTimeoutMs;
        _jumpActive = true;

        Log($"[jumptest] starting {req.JumpCount} discrete jump(s), {req.PressDurationMs}ms press each, lockMode={(lockKind.HasValue ? lockKind.Value.ToString() : "none")}, " +
            "buttonMask=IN_JUMP(bit1, HYPOTHESIS) only. Each jump waits for a confirmed/timed-out grounded state first, per criterion D.");
    }

    private enum JumpPhase { WaitGroundedBefore, Sampling, IdleHold }

    private const long JumpGroundWaitTimeoutMs = 2000;
    private const long JumpSampleMs = 900;     // long enough to capture a full CS2 jump arc and landing
    private const long JumpIdleHoldMs = 600;   // extra window with no injection, to catch a stuck jump button

    private bool _jumpActive;
    private int _jumpSlot;
    private uint _jumpPawnHandleRaw;
    private LockKind? _jumpLockKind;
    private int _jumpCountRequested;
    private int _jumpAttemptsDone;
    private int _jumpPressDurationMs;
    private JumpPhase _jumpPhase;
    private long _jumpPhaseDeadlineMs;
    private int _jumpAttemptPreTick;
    private float _jumpAttemptPreZ;
    private float _jumpAttemptPeakVelZ;
    private float _jumpAttemptPeakZ;
    private readonly List<long> _jumpInjectionIds = new();
    private readonly List<(long tMs, string note, bool onGround, int lastJumpTick, float velZ, float z)> _jumpTrace = new();

    private void TickJumpTest()
    {
        if (!_jumpActive) return;

        CCSPlayerPawn pawn;
        try
        {
            var (_, p, why) = ResolveLiveBotPawn(_jumpSlot);
            if (p is null) { FinishJumpTest(aborted: true, $"pawn no longer resolvable ({why})"); return; }
            if (p.EntityHandle.Raw != _jumpPawnHandleRaw) { FinishJumpTest(aborted: true, "pawn entity changed mid-test"); return; }
            pawn = p;
        }
        catch (Exception ex) { FinishJumpTest(aborted: true, $"pawn re-resolve threw {ex.GetType().Name}: {ex.Message}"); return; }

        var api = BotControllerCap.Get();
        if (api is null) { FinishJumpTest(aborted: true, "capability unavailable mid-test"); return; }

        long now = MonotonicMs();
        var (jOk, onGround, lastJumpTick, jDetail) = ReadJumpState(pawn);
        var (pOk, origin, _, pDetail) = ReadPawnState(pawn);
        var (vOk, velocity, vDetail) = ReadPawnVelocity(pawn);
        float z = pOk ? origin!.Z : 0f;
        float velZ = vOk ? velocity!.Z : 0f;

        switch (_jumpPhase)
        {
            case JumpPhase.WaitGroundedBefore:
                bool timedOut = now >= _jumpPhaseDeadlineMs;
                if (!jOk || !pOk)
                {
                    _jumpTrace.Add((now, $"WaitGrounded READ FAILED (jump:{jDetail} pos:{pDetail})", false, -1, 0, 0));
                    if (!timedOut) break;
                }
                if (!onGround && !timedOut) break; // keep waiting (bounded)

                _jumpTrace.Add((now, onGround ? "grounded confirmed -- jumping" : "grounded NOT confirmed within timeout -- jumping anyway",
                    onGround, lastJumpTick, velZ, z));
                _jumpAttemptPreTick = lastJumpTick;
                _jumpAttemptPreZ = z;
                _jumpAttemptPeakVelZ = velZ;
                _jumpAttemptPeakZ = z;

                long injId;
                try { injId = api.InjectUsercmd(_jumpSlot, MotorTestSafety.JumpButtonMask, _jumpPressDurationMs); }
                catch (Exception ex) { Log($"[jumptest] InjectUsercmd threw {ex.GetType().Name}: {ex.Message}"); injId = -1; }
                _jumpInjectionIds.Add(injId);
                Log($"[jumptest] jump #{_jumpAttemptsDone + 1}/{_jumpCountRequested}: InjectUsercmd(buttonMask=IN_JUMP, durationMs={_jumpPressDurationMs}) returned id={injId} " +
                    (injId < 0 ? "(REJECTED)" : "(ACCEPTED -- lifecycle only, not proof of an actual jump)"));

                _jumpPhase = JumpPhase.Sampling;
                _jumpPhaseDeadlineMs = now + JumpSampleMs;
                break;

            case JumpPhase.Sampling:
                if (jOk && pOk)
                {
                    _jumpAttemptPeakVelZ = MathF.Max(_jumpAttemptPeakVelZ, velZ);
                    _jumpAttemptPeakZ = MathF.Max(_jumpAttemptPeakZ, z);
                    _jumpTrace.Add((now, "sampling", onGround, lastJumpTick, velZ, z));
                }
                if (now < _jumpPhaseDeadlineMs) break;

                bool tickAdvanced = jOk && lastJumpTick != _jumpAttemptPreTick && lastJumpTick >= 0;
                float heightGain = _jumpAttemptPeakZ - _jumpAttemptPreZ;
                Log($"[jumptest] jump #{_jumpAttemptsDone + 1} result: LastJumpTick {_jumpAttemptPreTick}->observed-changed={tickAdvanced}, " +
                    $"peakVelZ={_jumpAttemptPeakVelZ:F2}, heightGain={heightGain:F4}, re-grounded-by-end={onGround} -- " +
                    (tickAdvanced && _jumpAttemptPeakVelZ > 50f && heightGain > 2f
                        ? "JUMP VERDICT: PASS (engine-recorded jump + upward velocity + measurable height gain)"
                        : "JUMP VERDICT: FAIL (missing one or more of: LastJumpTick advance, positive vertical velocity, measurable height gain)"));

                _jumpAttemptsDone++;
                if (_jumpAttemptsDone >= _jumpCountRequested)
                {
                    _jumpPhase = JumpPhase.IdleHold;
                    _jumpPhaseDeadlineMs = now + JumpIdleHoldMs;
                    break;
                }
                _jumpPhase = JumpPhase.WaitGroundedBefore;
                _jumpPhaseDeadlineMs = now + JumpGroundWaitTimeoutMs;
                break;

            case JumpPhase.IdleHold:
                if (now < _jumpPhaseDeadlineMs) break;
                FinishJumpTest(aborted: false, null);
                break;
        }
    }

    private void FinishJumpTest(bool aborted, string? abortReason)
    {
        _jumpActive = false;
        Log($"[jumptest] slot={_jumpSlot} requested={_jumpCountRequested} completed={_jumpAttemptsDone} " +
            $"lockMode={(_jumpLockKind.HasValue ? _jumpLockKind.Value.ToString() : "none")}" + (aborted ? $" -- ABORTED: {abortReason}" : ""));
        foreach (var t in _jumpTrace)
            Log($"[jumptest] t={t.tMs} {t.note} onGround={t.onGround} lastJumpTick={t.lastJumpTick} velZ={t.velZ:F2} z={t.z:F4}");
        if (!aborted)
            Log("[jumptest] RELEASE-SEMANTICS: idle-hold window completed with no further injection -- see per-jump VERDICT lines above for " +
                "whether the bit 1<<1 hypothesis was confirmed by actual engine jump evidence.");

        var api = BotControllerCap.Get();
        foreach (var id in _jumpInjectionIds)
        {
            if (id < 0) continue;
            try { api?.CancelUsercmdInjection(_jumpSlot, id); } catch { /* best-effort */ }
        }
    }

    private void ClearJumpTest(string reason)
    {
        if (!_jumpActive) return;
        _jumpActive = false;
        var api = BotControllerCap.Get();
        foreach (var id in _jumpInjectionIds)
        {
            if (id < 0) continue;
            try { api?.CancelUsercmdInjection(_jumpSlot, id); } catch { /* best-effort */ }
        }
        Log($"[jumptest] cleared without finishing for slot {_jumpSlot}: {reason}");
    }

    // ---- CROUCH: css_poc_crouchtest -- same architecture again. IN_DUCK=1<<2
    // is a HYPOTHESIS until Ducked/DuckAmount independently confirm it.
    private static (bool ok, bool ducked, bool ducking, float duckAmount, string detail) ReadCrouchState(CCSPlayerPawn pawn)
    {
        try
        {
            var ms = pawn.MovementServices;
            if (ms is null) return (false, false, false, 0f, "no MovementServices");
            return (true, ms.Ducked, ms.Ducking, ms.DuckAmount, "ok");
        }
        catch (Exception ex) { return (false, false, false, 0f, $"unreadable:{ex.GetType().Name}"); }
    }

    [ConsoleCommand("css_poc_crouchtest", "CROUCH: inject/hold candidate IN_DUCK bit and verify via independently-observed Ducked/DuckAmount (diagnostic-only, isolated-server-only)")]
    [CommandHelper(minArgs: 1, usage: "<slot> [holdCount=2] [holdMs=800] [lockMode=none|aim|all, default none]", whoCanExecute: CommandUsage.CLIENT_AND_SERVER)]
    public void OnCrouchTest(CCSPlayerController? caller, CommandInfo cmd)
    {
        if (!_isolatedEnvironmentConfirmed)
        {
            Log($"[crouchtest] REFUSED: isolated-server environment not confirmed (ModuleDirectory={ModuleDirectory}) -- failing closed.");
            return;
        }
        ReapExpiredTeleportVerification();
        if (_teleportVerifyPending)
        {
            Log($"[crouchtest] REJECTED: a css_poc_teleport verification is still pending on slot {_teleportVerifySlot} -- retry shortly.");
            return;
        }
        if (_crouchActive)
        {
            Log($"[crouchtest] REJECTED: a previous css_poc_crouchtest is still running on slot {_crouchSlot} -- wait, or css_poc_stop {_crouchSlot}.");
            return;
        }

        var rawArgs = new string?[3];
        for (int i = 0; i < rawArgs.Length; i++) rawArgs[i] = i + 1 < cmd.ArgCount ? cmd.GetArg(i + 1) : null;
        if (!MotorTestSafety.TryParseCrouchTestArgs(rawArgs, out var req, out var parseError))
        { Log($"[crouchtest] {parseError}"); return; }

        var (_, pawn, why) = ResolveLiveBotPawn(req.Slot);
        if (pawn is null) { Log($"[crouchtest] REFUSED: slot {req.Slot} {why}"); return; }
        var api = BotControllerCap.Get();
        if (api is null) { Log("[crouchtest] FAIL: capability not available (run gate1 first)"); return; }

        LockKind? lockKind = req.LockMode switch { FireLockMode.Aim => LockKind.Aim, FireLockMode.All => LockKind.All, _ => null };
        if (lockKind.HasValue)
        {
            bool locked;
            try { locked = api.Lock(req.Slot, lockKind.Value); }
            catch (Exception ex) { Log($"[crouchtest] Lock threw {ex.GetType().Name}: {ex.Message}"); return; }
            Log($"[crouchtest] Lock({lockKind.Value}) accepted={locked} (requested suppression tier for this test)");
        }
        else
        {
            Log("[crouchtest] lockMode=none -- no Lock call issued; testing whether injected crouch works with ZERO suppression.");
        }

        var (okPre, duckedPre, duckingPre, amountPre, detailPre) = ReadCrouchState(pawn);
        if (!okPre)
        {
            Log($"[crouchtest] REFUSED: cannot independently establish starting duck state ({detailPre}) -- aborting.");
            return;
        }
        Log($"[crouchtest] PRE slot={req.Slot} ducked={duckedPre} ducking={duckingPre} duckAmount={amountPre:F4}");

        uint pawnHandleRaw;
        try { pawnHandleRaw = pawn.EntityHandle.Raw; }
        catch (Exception ex) { Log($"[crouchtest] REFUSED: could not read pawn entity handle ({ex.GetType().Name}: {ex.Message})"); return; }

        _crouchSlot = req.Slot;
        _crouchPawnHandleRaw = pawnHandleRaw;
        _crouchLockKind = lockKind;
        _crouchHoldCountRequested = req.HoldCount;
        _crouchHoldsDone = 0;
        _crouchHoldMs = req.HoldMs;
        _crouchTrace.Clear();
        _crouchInjectionIds.Clear();
        _crouchPhase = CrouchPhase.Idle;
        _crouchPhaseDeadlineMs = MonotonicMs();
        _crouchActive = true;

        Log($"[crouchtest] starting {req.HoldCount} discrete hold(s), {req.HoldMs}ms each, lockMode={(lockKind.HasValue ? lockKind.Value.ToString() : "none")}, " +
            "buttonMask=IN_DUCK(bit2, HYPOTHESIS) only.");
    }

    private enum CrouchPhase { Idle, Holding, ReleaseCheck, FinalIdle }

    private const long CrouchGapMs = 400;        // between holds, and before starting the first
    private const long CrouchReleaseCheckMs = 500; // after a hold ends, window to confirm it un-crouches

    private bool _crouchActive;
    private int _crouchSlot;
    private uint _crouchPawnHandleRaw;
    private LockKind? _crouchLockKind;
    private int _crouchHoldCountRequested;
    private int _crouchHoldsDone;
    private int _crouchHoldMs;
    private CrouchPhase _crouchPhase;
    private long _crouchPhaseDeadlineMs;
    private bool _crouchReachedDuring;
    private readonly List<long> _crouchInjectionIds = new();
    private readonly List<(long tMs, string note, bool ducked, bool ducking, float duckAmount)> _crouchTrace = new();

    private void TickCrouchTest()
    {
        if (!_crouchActive) return;

        CCSPlayerPawn pawn;
        try
        {
            var (_, p, why) = ResolveLiveBotPawn(_crouchSlot);
            if (p is null) { FinishCrouchTest(aborted: true, $"pawn no longer resolvable ({why})"); return; }
            if (p.EntityHandle.Raw != _crouchPawnHandleRaw) { FinishCrouchTest(aborted: true, "pawn entity changed mid-test"); return; }
            pawn = p;
        }
        catch (Exception ex) { FinishCrouchTest(aborted: true, $"pawn re-resolve threw {ex.GetType().Name}: {ex.Message}"); return; }

        var api = BotControllerCap.Get();
        if (api is null) { FinishCrouchTest(aborted: true, "capability unavailable mid-test"); return; }

        long now = MonotonicMs();
        var (ok, ducked, ducking, amount, detail) = ReadCrouchState(pawn);
        if (ok) _crouchTrace.Add((now, _crouchPhase.ToString(), ducked, ducking, amount));

        switch (_crouchPhase)
        {
            case CrouchPhase.Idle:
                if (now < _crouchPhaseDeadlineMs) break;
                long injId;
                try { injId = api.InjectUsercmd(_crouchSlot, MotorTestSafety.DuckButtonMask, _crouchHoldMs); }
                catch (Exception ex) { Log($"[crouchtest] InjectUsercmd threw {ex.GetType().Name}: {ex.Message}"); injId = -1; }
                _crouchInjectionIds.Add(injId);
                _crouchReachedDuring = false;
                Log($"[crouchtest] hold #{_crouchHoldsDone + 1}/{_crouchHoldCountRequested}: InjectUsercmd(buttonMask=IN_DUCK, durationMs={_crouchHoldMs}) returned id={injId} " +
                    (injId < 0 ? "(REJECTED)" : "(ACCEPTED -- lifecycle only, not proof of an actual crouch)"));
                _crouchPhase = CrouchPhase.Holding;
                _crouchPhaseDeadlineMs = now + _crouchHoldMs;
                break;

            case CrouchPhase.Holding:
                if (ok && (ducked || ducking || amount > 0.1f)) _crouchReachedDuring = true;
                if (now < _crouchPhaseDeadlineMs) break;
                Log($"[crouchtest] hold #{_crouchHoldsDone + 1} end-of-hold: ducked={ducked} ducking={ducking} duckAmount={amount:F4} " +
                    $"reachedDuckedAtSomePoint={_crouchReachedDuring} -- " +
                    (_crouchReachedDuring ? "CROUCH VERDICT: PASS (independent Ducked/DuckAmount state confirms an actual crouch)"
                                           : "CROUCH VERDICT: FAIL (Ducked/Ducking/DuckAmount never left the resting state during the hold)"));
                _crouchPhase = CrouchPhase.ReleaseCheck;
                _crouchPhaseDeadlineMs = now + CrouchReleaseCheckMs;
                break;

            case CrouchPhase.ReleaseCheck:
                if (now < _crouchPhaseDeadlineMs) break;
                Log($"[crouchtest] hold #{_crouchHoldsDone + 1} release check: ducked={ducked} ducking={ducking} duckAmount={amount:F4} -- " +
                    (!ducked && amount < 0.5f ? "RELEASE VERDICT: PASS (returned toward un-crouched after hold ended)"
                                               : "RELEASE VERDICT: FAIL or STILL SETTLING (still substantially ducked after the release-check window -- inspect trace)"));
                _crouchHoldsDone++;
                if (_crouchHoldsDone >= _crouchHoldCountRequested)
                {
                    _crouchPhase = CrouchPhase.FinalIdle;
                    _crouchPhaseDeadlineMs = now + CrouchReleaseCheckMs;
                    break;
                }
                _crouchPhase = CrouchPhase.Idle;
                _crouchPhaseDeadlineMs = now + CrouchGapMs;
                break;

            case CrouchPhase.FinalIdle:
                if (now < _crouchPhaseDeadlineMs) break;
                FinishCrouchTest(aborted: false, null);
                break;
        }
    }

    private void FinishCrouchTest(bool aborted, string? abortReason)
    {
        _crouchActive = false;
        Log($"[crouchtest] slot={_crouchSlot} requested={_crouchHoldCountRequested} completed={_crouchHoldsDone} " +
            $"lockMode={(_crouchLockKind.HasValue ? _crouchLockKind.Value.ToString() : "none")}" + (aborted ? $" -- ABORTED: {abortReason}" : ""));
        foreach (var t in _crouchTrace)
            Log($"[crouchtest] t={t.tMs} phase={t.note} ducked={t.ducked} ducking={t.ducking} duckAmount={t.duckAmount:F4}");

        var api = BotControllerCap.Get();
        foreach (var id in _crouchInjectionIds)
        {
            if (id < 0) continue;
            try { api?.CancelUsercmdInjection(_crouchSlot, id); } catch { /* best-effort */ }
        }
    }

    private void ClearCrouchTest(string reason)
    {
        if (!_crouchActive) return;
        _crouchActive = false;
        var api = BotControllerCap.Get();
        foreach (var id in _crouchInjectionIds)
        {
            if (id < 0) continue;
            try { api?.CancelUsercmdInjection(_crouchSlot, id); } catch { /* best-effort */ }
        }
        Log($"[crouchtest] cleared without finishing for slot {_crouchSlot}: {reason}");
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
        // A css_poc_teleport's delayed verification reads the pawn a few ticks
        // after the teleport; starting Gate2 (lock/movement) inside that window
        // would confound both results, exactly as already guarded against in
        // OnGate2C below. Expired pending state is reaped first so it can
        // never block Gate2 indefinitely.
        ReapExpiredTeleportVerification();
        if (_teleportVerifyPending)
        {
            Log($"[gate2] REJECTED: a css_poc_teleport verification is still pending on slot {_teleportVerifySlot} -- " +
                $"retry in a moment (or css_poc_stop {_teleportVerifySlot}).");
            return;
        }

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

        // css_poc_teleport: a map transition invalidates every entity, so any
        // pending delayed verification is dropped rather than carried over.
        RegisterListener<Listeners.OnMapStart>(_ => ClearTeleportVerification("map start"));
        RegisterListener<Listeners.OnMapEnd>(() => ClearTeleportVerification("map end"));
        RegisterListener<Listeners.OnMapStart>(_ => ClearAimVerification("map start"));
        RegisterListener<Listeners.OnMapEnd>(() => ClearAimVerification("map end"));
        RegisterListener<Listeners.OnMapStart>(_ => ClearFireTest("map start"));
        RegisterListener<Listeners.OnMapEnd>(() => ClearFireTest("map end"));
        RegisterListener<Listeners.OnMapStart>(_ => ClearJumpTest("map start"));
        RegisterListener<Listeners.OnMapEnd>(() => ClearJumpTest("map end"));
        RegisterListener<Listeners.OnMapStart>(_ => ClearCrouchTest("map start"));
        RegisterListener<Listeners.OnMapEnd>(() => ClearCrouchTest("map end"));

        // Gate2C-only precision-control extension: round-transition detection.
        // Purely a counter -- this never blocks, cancels, or alters a running
        // test, it only lets a test's own finalize log state whether a round
        // boundary happened during its window, so a confounded result can be
        // flagged rather than silently trusted. Independent of every existing
        // gate/command in this file.
        RegisterEventHandler<EventRoundStart>((@event, info) =>
        {
            _roundTransitionCounter++;
            return HookResult.Continue;
        });
        RegisterEventHandler<EventRoundEnd>((@event, info) =>
        {
            _roundTransitionCounter++;
            return HookResult.Continue;
        });
    }

    private long _roundTransitionCounter;

    private void OnTick()
    {
        TickTeleportVerification();
        TickAimVerification();
        TickFireTest();
        TickJumpTest();
        TickCrouchTest();
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
        if (_gate2cWaitingForRest && _gate2cWaitPawn is not null)
        {
            var (velOk, velocity, _) = ReadPawnVelocity(_gate2cWaitPawn);
            bool atRest = velOk && velocity is not null && VectorMagnitude(velocity) < Gate2CRestVelocityThreshold;
            _gate2cWaitRestTicks = atRest ? _gate2cWaitRestTicks + 1 : 0;

            if (_gate2cWaitRestTicks >= Gate2CRestConsecutiveTicksRequired)
            {
                _gate2cWaitingForRest = false;
                Log($"[gate2c] rest confirmed for slot {_gate2cWaitSlot} ({_gate2cWaitRestTicks} consecutive ticks below " +
                    $"{Gate2CRestVelocityThreshold} u/s) -- starting capture.");
                StartGate2CDiagnostic(_gate2cWaitSlot, _gate2cWaitDurationMs, _gate2cWaitWriteScale, _gate2cWaitLockKind,
                    _gate2cWaitGate2cOnly, _gate2cWaitForwardMove, _gate2cWaitLeftMove, _gate2cWaitReverseAtMs,
                    _gate2cWaitPawn, preLockedResult: _gate2cWaitLocked);
            }
            else if (MonotonicMs() >= _gate2cWaitDeadlineMs)
            {
                _gate2cWaitingForRest = false;
                Log($"[gate2c] TIMEOUT waiting for rest on slot {_gate2cWaitSlot} -- no diagnostic was started, " +
                    "nothing to finalize; releasing lock now.");
                var api = BotControllerCap.Get();
                try { api?.Unlock(_gate2cWaitSlot, _gate2cWaitLockKind); } catch { /* diagnostic-only, fail soft */ }
            }
        }
        if (_gate2cActive && _gate2cPawn is not null)
        {
            var (ok, origin, eye, _) = ReadPawnState(_gate2cPawn);
            var (velOk, velocity, _) = ReadPawnVelocity(_gate2cPawn);
            if (ok) _gate2cSamples.Add((MonotonicMs(), origin!.X, origin.Y, origin.Z, eye!.X, eye.Y,
                velOk, velocity?.X ?? 0f, velocity?.Y ?? 0f, velocity?.Z ?? 0f));

            if (!_gate2cReversed && _gate2cReverseAtMs > 0 && MonotonicMs() >= _gate2cReverseAtAbsoluteMs)
            {
                _gate2cReversed = true;
                var api = BotControllerCap.Get();
                if (api is not null && _activeMovement.TryGetValue(_gate2cSlot, out var reverseId))
                {
                    try
                    {
                        api.UpdateUsercmdMovement(_gate2cSlot, reverseId, -_gate2cForwardMove, -_gate2cLeftMove);
                        Log($"[gate2c] reversal applied at t={MonotonicMs()} (elapsed={MonotonicMs() - _gate2cStartMs}ms): " +
                            $"forward {_gate2cForwardMove:F4}->{-_gate2cForwardMove:F4}, left {_gate2cLeftMove:F4}->{-_gate2cLeftMove:F4}.");
                    }
                    catch (Exception ex) { Log($"[gate2c] reversal UpdateUsercmdMovement threw {ex.GetType().Name}: {ex.Message}"); }
                }
                else
                {
                    Log("[gate2c] reversal requested but no active movement id was found -- skipped.");
                }
            }

            if (MonotonicMs() >= _gate2cDeadlineMs)
            {
                _gate2cActive = false;
                BeginGate2CPostCancel();
            }
        }
        if (_gate2cPostCancelActive && _gate2cPawn is not null)
        {
            var (ok, origin, eye, _) = ReadPawnState(_gate2cPawn);
            var (velOk, velocity, _) = ReadPawnVelocity(_gate2cPawn);
            if (ok) _gate2cPostCancelSamples.Add((MonotonicMs(), origin!.X, origin.Y, origin.Z, eye!.X, eye.Y,
                velOk, velocity?.X ?? 0f, velocity?.Y ?? 0f, velocity?.Z ?? 0f));
            if (MonotonicMs() >= _gate2cPostCancelDeadlineMs)
            {
                _gate2cPostCancelActive = false;
                FinishGate2C(aborted: false);
            }
        }
    }

    private static float VectorMagnitude(Vector v) => MathF.Sqrt(v.X * v.X + v.Y * v.Y + v.Z * v.Z);

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

    // ---- GATE 2C: like GATE2 above (unmodified), but ALSO starts the Gate2C
    // native diagnostic, which additionally writes the same shared movement
    // intent directly into CMoveData inside the ProcessMovement pre-hook and
    // records PRE/INJECTED/POST for each invocation. Runs its OWN Gate2B
    // window too (via the existing, unmodified TryFinalizeGate2Diagnostic/
    // _gate2FinalizedGuard), so one test produces directly comparable Gate2B
    // and Gate2C native logs. Entirely separate state/fields from css_poc_gate2
    // above -- that command is untouched.
    [ConsoleCommand("css_poc_gate2c", "GATE2C: like GATE2, but also writes movement directly into CMoveData in ProcessMovement (diagnostic-only, explicit writeScale)")]
    [CommandHelper(minArgs: 3, usage: "<slot> <durationMs> <writeScale> [lockMode: all|aim, default all] [gate2cOnly: true|false, default false] [forwardMove, default 1.0] [leftMove, default 0.0] [reverseAtMs, default disabled] [waitForRestMs, default disabled]", whoCanExecute: CommandUsage.CLIENT_AND_SERVER)]
    public void OnGate2C(CCSPlayerController? caller, CommandInfo cmd)
    {
        // Admission guard, harness-side only: css_poc_gate2c's own state
        // (_gate2cWaitingForRest/_gate2cActive/_gate2cPostCancelActive) is a
        // single, non-per-slot set of fields -- always has been, even before
        // the rest-wait phase existed. A second invocation while any of these
        // is still true would silently overwrite _gate2cSlot/_gate2cPawn/etc
        // out from under the still-running session: its OnTick sampling would
        // start reading the WRONG pawn, its deadline/reversal checks would
        // fire against the new session's timing, and the original session's
        // Lock/movement could be left ownerless. This does not touch native
        // Gate2C-only ownership/exclusivity at all (that is already correct
        // and unaffected) -- it only stops the HARNESS's own shared bookkeeping
        // from being clobbered. Global rather than per-slot, matching the
        // existing single-session-at-a-time architecture of this command
        // (not a new limitation -- see the diagnostic below for the busy slot).
        if (_gate2cWaitingForRest || _gate2cActive || _gate2cPostCancelActive)
        {
            int busySlot = _gate2cWaitingForRest ? _gate2cWaitSlot : _gate2cSlot;
            Log($"[gate2c] REJECTED: another Gate2C session (or a pending rest-wait) is already active on slot " +
                $"{busySlot} -- call css_poc_stop {busySlot} first, or wait for it to finish, before starting a new one.");
            return;
        }

        // A css_poc_teleport's delayed verification reads the pawn a few ticks
        // after the teleport; starting Gate2C (lock/movement) inside that window
        // would confound both results. Expired pending state is reaped first so
        // it can never block Gate2C indefinitely.
        ReapExpiredTeleportVerification();
        if (_teleportVerifyPending)
        {
            Log($"[gate2c] REJECTED: a css_poc_teleport verification is still pending on slot {_teleportVerifySlot} -- " +
                $"retry in a moment (or css_poc_stop {_teleportVerifySlot}).");
            return;
        }

        if (!int.TryParse(cmd.GetArg(1), out var slot) || !int.TryParse(cmd.GetArg(2), out var durationMs))
        { Log("[gate2c] bad slot/durationMs args"); return; }
        if (!float.TryParse(cmd.GetArg(3), System.Globalization.NumberStyles.Float,
                System.Globalization.CultureInfo.InvariantCulture, out var writeScale))
        {
            Log("[gate2c] bad writeScale arg -- must be a number, e.g. 1.0. This is NOT defaulted (see " +
                "Gate2CDiagnostics.h): the caller must state the value under test explicitly, and neither " +
                "450 nor 1.0 is asserted here as an established CMoveData unit.");
            return;
        }

        var lockModeArg = cmd.GetArg(4);
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
            Log($"[gate2c] bad lockMode arg '{lockModeArg}' -- must be 'all' or 'aim' (omit for default 'all')");
            return;
        }

        var gate2cOnlyArg = cmd.GetArg(5);
        bool gate2cOnly = false;
        if (!string.IsNullOrWhiteSpace(gate2cOnlyArg) && !bool.TryParse(gate2cOnlyArg, out gate2cOnly))
        {
            Log("[gate2c] bad gate2cOnly arg -- use true or false");
            return;
        }
        if (gate2cOnly && (durationMs < 1 || (long)durationMs + Gate2PostCancelDurationMs + 2000 > 60000))
        {
            Log("[gate2c] gate2cOnly requires a positive duration with room for the post-cancel safety margin (max 57000ms)");
            return;
        }

        // ---- New, all-optional args. Every default below reproduces today's
        // exact behavior when omitted -- backward compatible by construction.
        float forwardMove = 1.0f;
        var forwardMoveArg = cmd.GetArg(6);
        if (!string.IsNullOrWhiteSpace(forwardMoveArg) && (!float.TryParse(forwardMoveArg, System.Globalization.NumberStyles.Float,
                System.Globalization.CultureInfo.InvariantCulture, out forwardMove) || !float.IsFinite(forwardMove)))
        { Log("[gate2c] bad forwardMove arg -- must be a finite number, e.g. 1.0 or -1.0 (omit for default 1.0)"); return; }

        float leftMove = 0.0f;
        var leftMoveArg = cmd.GetArg(7);
        if (!string.IsNullOrWhiteSpace(leftMoveArg) && (!float.TryParse(leftMoveArg, System.Globalization.NumberStyles.Float,
                System.Globalization.CultureInfo.InvariantCulture, out leftMove) || !float.IsFinite(leftMove)))
        { Log("[gate2c] bad leftMove arg -- must be a finite number, e.g. 1.0 or -1.0 (omit for default 0.0)"); return; }

        int reverseAtMs = -1;
        var reverseAtMsArg = cmd.GetArg(8);
        if (!string.IsNullOrWhiteSpace(reverseAtMsArg))
        {
            if (!int.TryParse(reverseAtMsArg, out reverseAtMs) || reverseAtMs <= 0 || reverseAtMs >= durationMs)
            { Log("[gate2c] bad reverseAtMs arg -- must be a positive integer strictly less than durationMs (omit to disable reversal)"); return; }
        }

        int waitForRestMs = 0;
        var waitForRestMsArg = cmd.GetArg(9);
        if (!string.IsNullOrWhiteSpace(waitForRestMsArg))
        {
            if (!int.TryParse(waitForRestMsArg, out waitForRestMs) || waitForRestMs < 0)
            { Log("[gate2c] bad waitForRestMs arg -- must be a non-negative integer (0 or omit disables the rest-wait)"); return; }
        }

        var api = BotControllerCap.Get();
        if (api is null) { Log("[gate2c] FAIL: capability not available (run gate1 first)"); return; }
        var controller = ResolveSlot(slot);
        if (controller is null || !controller.IsBot) { Log($"[gate2c] slot {slot} is not a live bot"); return; }
        var pawn = controller.PlayerPawn?.Value;
        if (pawn is null) { Log($"[gate2c] slot {slot} has no pawn"); return; }

        var (okPre, originPre, eyePre, detailPre) = ReadPawnState(pawn);
        Log(okPre
            ? $"[gate2c] PRE origin=({originPre!.X:F4},{originPre.Y:F4},{originPre.Z:F4}) eye=({eyePre!.X:F4},{eyePre.Y:F4})"
            : $"[gate2c] PRE read FAILED: {detailPre} -- aborting, cannot establish baseline");
        if (!okPre) return;

        if (waitForRestMs <= 0)
        {
            // Fast path: identical sequencing to before this change (Gate2B
            // Start -> Gate2C Start -> Lock -> StartUsercmdMovement), entirely
            // inside StartGate2CDiagnostic with preLockedResult=null.
            StartGate2CDiagnostic(slot, durationMs, writeScale, lockKind, gate2cOnly, forwardMove, leftMove,
                reverseAtMs, pawn, preLockedResult: null);
            return;
        }

        // Rest-wait path: Lock now, BEFORE Gate2B/Gate2C Start and before any
        // movement injection, so AI stops making new decisions and residual
        // momentum can decay via ordinary friction while we wait. Only
        // reachable when the caller explicitly opts in via waitForRestMs -- no
        // existing invocation without this argument is affected.
        bool locked;
        try { locked = api.Lock(slot, lockKind); }
        catch (Exception ex) { Log($"[gate2c] Lock threw {ex.GetType().Name}: {ex.Message}"); return; }
        Log($"[gate2c] Lock({lockKind}) accepted={locked}");

        _gate2cWaitSlot = slot;
        _gate2cWaitPawn = pawn;
        _gate2cWaitLockKind = lockKind;
        _gate2cWaitLocked = locked;
        _gate2cWaitDurationMs = durationMs;
        _gate2cWaitWriteScale = writeScale;
        _gate2cWaitGate2cOnly = gate2cOnly;
        _gate2cWaitForwardMove = forwardMove;
        _gate2cWaitLeftMove = leftMove;
        _gate2cWaitReverseAtMs = reverseAtMs;
        _gate2cWaitRestTicks = 0;
        _gate2cWaitDeadlineMs = MonotonicMs() + waitForRestMs;
        _gate2cWaitingForRest = true;
        Log($"[gate2c] waiting up to {waitForRestMs}ms for slot {slot} to reach rest (|velocity| < " +
            $"{Gate2CRestVelocityThreshold} u/s for {Gate2CRestConsecutiveTicksRequired} consecutive ticks) before " +
            "starting capture. Call css_poc_stop early to abort this wait too.");
    }

    // Shared continuation for both OnGate2C's fast path (preLockedResult=null:
    // this function performs Lock itself, in the exact same position in the
    // sequence -- after Gate2C DiagnosticStart -- as before this change) and
    // the rest-wait path (preLockedResult=the already-obtained Lock result:
    // Lock is skipped here since the caller already did it earlier). Every
    // failure/exception path below unlocks (when locked) and finalizes both
    // native diagnostics (when they were started) before returning -- no exit
    // path leaves the lock held or a diagnostic un-finalized.
    private void StartGate2CDiagnostic(int slot, int durationMs, float writeScale, LockKind lockKind, bool gate2cOnly,
        float forwardMove, float leftMove, int reverseAtMs, CCSPlayerPawn pawn, bool? preLockedResult)
    {
        var api = BotControllerCap.Get();
        if (api is null)
        {
            // preLockedResult != null means a Lock call already succeeded (or was
            // attempted) earlier, via a DIFFERENT api reference (the rest-wait
            // setup in OnGate2C, which may have run up to waitForRestMs ago) --
            // if the capability is gone now, there is no api object left to call
            // Unlock on through this path. This is a real, narrow gap: unlike the
            // fast path (Lock and Start share one api reference within a single
            // call), the rest-wait path's Lock and this continuation are separate
            // calls with a real time gap between them. Not silently swallowed:
            // logged distinctly so a stuck-locked bot has a clear cause in the log
            // rather than the generic capability-unavailable message.
            if (preLockedResult == true)
                Log($"[gate2c] FAIL: capability not available (run gate1 first) -- slot {slot} may STILL BE LOCKED " +
                    $"from the earlier rest-wait Lock({lockKind}) call and cannot be released via this path; " +
                    "retry css_poc_stop once the capability is available again.");
            else
                Log("[gate2c] FAIL: capability not available (run gate1 first)");
            return;
        }

        System.Threading.Interlocked.Exchange(ref _gate2FinalizedGuard, 0);
        int startStatusB;
        try { startStatusB = BotController_Gate2BDiagnosticStart(slot); }
        catch (Exception ex) { startStatusB = -1; Log($"[gate2c] Gate2B DiagnosticStart threw {ex.GetType().Name}: {ex.Message} -- proceeding, Gate2B side of this run will be unreliable."); }
        if (startStatusB == 3) Log("[gate2c] Gate2B DiagnosticStart returned BUSY -- Gate2B side of this run will be unreliable.");

        System.Threading.Interlocked.Exchange(ref _gate2cFinalizedGuard, 0);
        int startStatusC;
        try { startStatusC = BotController_Gate2CDiagnosticStart(slot, writeScale); }
        catch (Exception ex)
        {
            Log($"[gate2c] Gate2C DiagnosticStart threw {ex.GetType().Name}: {ex.Message} -- aborting.");
            if (preLockedResult == true) { try { api.Unlock(slot, lockKind); } catch { /* diagnostic-only, fail soft */ } }
            TryFinalizeBothGate2CDiagnostics(slot, aborted: true);
            return;
        }
        if (startStatusC != 0)
        {
            Log($"[gate2c] Gate2C DiagnosticStart returned status={startStatusC} (0=OK, 2=INVALID_SLOT, 3=BUSY) -- aborting rather than writing CMoveData with no reliable capture active.");
            if (preLockedResult == true) { try { api.Unlock(slot, lockKind); } catch { /* diagnostic-only, fail soft */ } }
            TryFinalizeBothGate2CDiagnostics(slot, aborted: true);
            return;
        }
        Log($"[gate2c] Gate2C capture started, writeScale={writeScale} (explicit, experimental).");

        bool locked;
        if (preLockedResult is bool already)
        {
            locked = already;
        }
        else
        {
            try { locked = api.Lock(slot, lockKind); }
            catch (Exception ex)
            {
                Log($"[gate2c] Lock threw {ex.GetType().Name}: {ex.Message}");
                TryFinalizeBothGate2CDiagnostics(slot, aborted: true);
                return;
            }
            Log($"[gate2c] Lock({lockKind}) accepted={locked}");
        }

        long movementId;
        try { movementId = gate2cOnly
            ? BotController_StartUsercmdMovementGate2COnly(slot, forwardMove, leftMove, checked((int)(durationMs + Gate2PostCancelDurationMs + 2000)))
            : api.StartUsercmdMovement(slot, forwardMove, leftMove); }
        catch (Exception ex)
        {
            Log($"[gate2c] StartUsercmdMovement threw {ex.GetType().Name}: {ex.Message}");
            if (locked) api.Unlock(slot, lockKind);
            TryFinalizeBothGate2CDiagnostics(slot, aborted: true);
            return;
        }
        Log($"[gate2c] StartUsercmdMovement accepted, id={movementId}");
        if (movementId < 0)
        {
            if (locked) api.Unlock(slot, lockKind);
            TryFinalizeBothGate2CDiagnostics(slot, aborted: true);
            return;
        }
        _activeMovement[slot] = movementId;

        _gate2cSamples.Clear();
        _gate2cSlot = slot;
        _gate2cPawn = pawn;
        _gate2cLockKind = lockKind;
        _gate2cStartMs = MonotonicMs();
        _gate2cDeadlineMs = _gate2cStartMs + durationMs;
        _gate2cForwardMove = forwardMove;
        _gate2cLeftMove = leftMove;
        _gate2cReverseAtMs = reverseAtMs;
        _gate2cReverseAtAbsoluteMs = reverseAtMs > 0 ? _gate2cStartMs + reverseAtMs : long.MaxValue;
        _gate2cReversed = false;
        _gate2cRoundTransitionCounterAtStart = _roundTransitionCounter;
        _gate2cActive = true;
        Log($"[gate2c] sampling started for {durationMs}ms with lockKind={lockKind}, writeScale={writeScale}, " +
            $"forwardMove={forwardMove:F4}, leftMove={leftMove:F4}" +
            (reverseAtMs > 0 ? $", reverseAtMs={reverseAtMs}" : "") +
            $". Call css_poc_stop {slot} early if needed; otherwise it self-stops and logs the full trace.");
    }

    // velOk/velX/velY/velZ are the independent entity-velocity read (AbsVelocity,
    // via ReadPawnVelocity) -- separate from and cross-checkable against
    // Gate2C's own native CMoveData postVel readback in the BotController log.
    private readonly List<(long tMs, float x, float y, float z, float pitch, float yaw, bool velOk, float velX, float velY, float velZ)> _gate2cSamples = new();
    private bool _gate2cActive;
    private int _gate2cSlot;
    private CCSPlayerPawn? _gate2cPawn;
    private LockKind _gate2cLockKind;
    private long _gate2cDeadlineMs;
    private long _gate2cStartMs;

    // Reversal (mid-run direction flip via the existing, unmodified native
    // UpdateUsercmdMovement -- see IBotControllerApi.UpdateUsercmdMovement):
    // reverseAtMs<=0 disables it entirely, preserving today's behavior.
    private float _gate2cForwardMove;
    private float _gate2cLeftMove;
    private int _gate2cReverseAtMs;
    private long _gate2cReverseAtAbsoluteMs;
    private bool _gate2cReversed;

    // Round-transition detection: sampled at StartGate2CDiagnostic and compared
    // at finalize. A mismatch means a round boundary happened during this run
    // and the result should be treated as confounded, not silently trusted.
    private long _gate2cRoundTransitionCounterAtStart;

    // Rest-wait (starting from rest): deferred continuation state. Lock is
    // already held by the time this phase is active (see OnGate2C) --
    // OnStop's existing unconditional Unlock(All)/Unlock(Aim) fail-safe
    // already covers releasing it if a manual stop happens mid-wait; this
    // phase's own timeout path releases it too (see OnTick).
    private const float Gate2CRestVelocityThreshold = 5.0f; // units/sec
    private const int Gate2CRestConsecutiveTicksRequired = 8;
    private bool _gate2cWaitingForRest;
    private int _gate2cWaitSlot;
    private CCSPlayerPawn? _gate2cWaitPawn;
    private LockKind _gate2cWaitLockKind;
    private bool _gate2cWaitLocked;
    private int _gate2cWaitDurationMs;
    private float _gate2cWaitWriteScale;
    private bool _gate2cWaitGate2cOnly;
    private float _gate2cWaitForwardMove;
    private float _gate2cWaitLeftMove;
    private int _gate2cWaitReverseAtMs;
    private long _gate2cWaitDeadlineMs;
    private int _gate2cWaitRestTicks;

    private readonly List<(long tMs, float x, float y, float z, float pitch, float yaw, bool velOk, float velX, float velY, float velZ)> _gate2cPostCancelSamples = new();
    private bool _gate2cPostCancelActive;
    private long _gate2cPostCancelDeadlineMs;

    // Cancels HOS movement, marks BOTH native diagnostics' phase transition,
    // and begins the bounded still-locked post-cancel observation window --
    // same structure as BeginGate2PostCancel (unmodified), duplicated rather
    // than parameterized so css_poc_gate2's own path is never at risk of
    // being altered by a Gate2C-motivated refactor.
    private void BeginGate2CPostCancel()
    {
        var api = BotControllerCap.Get();
        if (api is not null && _activeMovement.TryGetValue(_gate2cSlot, out var id))
        {
            try { api.CancelUsercmdMovement(_gate2cSlot, id); } catch { /* diagnostic-only, fail soft */ }
        }
        try { BotController_Gate2BDiagnosticMarkCancelled(_gate2cSlot); }
        catch (Exception ex) { Log($"[gate2c] Gate2B DiagnosticMarkCancelled threw {ex.GetType().Name}: {ex.Message}"); }
        try { BotController_Gate2CDiagnosticMarkCancelled(_gate2cSlot); }
        catch (Exception ex) { Log($"[gate2c] Gate2C DiagnosticMarkCancelled threw {ex.GetType().Name}: {ex.Message}"); }

        _gate2cPostCancelSamples.Clear();
        _gate2cPostCancelDeadlineMs = MonotonicMs() + Gate2PostCancelDurationMs;
        _gate2cPostCancelActive = true;
        Log($"[gate2c] movement cancelled; observing for {Gate2PostCancelDurationMs}ms while lockKind={_gate2cLockKind} remains active before unlock");
    }

    private int _gate2cFinalizedGuard;

    private void TryFinalizeGate2CDiagnostic(int slot, bool aborted)
    {
        if (System.Threading.Interlocked.CompareExchange(ref _gate2cFinalizedGuard, 1, 0) != 0)
        {
            Log("[gate2c] Gate2C finalize already claimed by another code path for this test -- skipping a duplicate, potentially contradictory local summary.");
            return;
        }
        int finalizeStatus;
        try { finalizeStatus = BotController_Gate2CDiagnosticFinalize(slot, aborted ? 1 : 0); }
        catch (Exception ex) { finalizeStatus = -1; Log($"[gate2c] Gate2C DiagnosticFinalize threw {ex.GetType().Name}: {ex.Message}"); }
        string label = finalizeStatus switch
        {
            0 => aborted ? "ABORTED" : "COMPLETE",
            1 => "REPEAT (native side already finalized this window independently)",
            2 => "INVALID_SLOT",
            3 => "BUSY/INCOMPLETE (native bounded drain did not confirm quiescence -- buffers were NOT read; do not treat this as a completed test)",
            _ => $"UNKNOWN(status={finalizeStatus})",
        };
        Log($"[gate2c] [{label}] Gate2C DiagnosticFinalize status={finalizeStatus} -- see native BotController log " +
            "for the bounded PRE/INJECTED/POST sample dump (Activation/SteadyState/Cancellation/PostCancel) and " +
            "pairing counters (overflow/unmatchedPost/orphanedFrames/staleGeneration), when status=0 or 1.");
    }

    // Finalizes BOTH the Gate2B and Gate2C native diagnostics for one
    // css_poc_gate2c test, each through its own single-owner guard (the
    // existing TryFinalizeGate2Diagnostic for Gate2B, unmodified; the new
    // TryFinalizeGate2CDiagnostic above for Gate2C), so every exit path --
    // normal completion, an early-return failure in OnGate2C, or an
    // emergency css_poc_stop -- leaves both native diagnostics in a defined,
    // logged state.
    private void TryFinalizeBothGate2CDiagnostics(int slot, bool aborted)
    {
        TryFinalizeGate2Diagnostic(slot, aborted);
        TryFinalizeGate2CDiagnostic(slot, aborted);
    }

    private void FinishGate2C(bool aborted)
    {
        TryFinalizeBothGate2CDiagnostics(_gate2cSlot, aborted);

        Log($"[gate2c] pre-cancel sample count={_gate2cSamples.Count} lockKind={_gate2cLockKind}");
        foreach (var s in _gate2cSamples)
            Log($"[gate2c] t={s.tMs} pos=({s.x:F4},{s.y:F4},{s.z:F4}) eye=(pitch={s.pitch:F4},yaw={s.yaw:F4}) " +
                (s.velOk ? $"vel=({s.velX:F4},{s.velY:F4},{s.velZ:F4})" : "vel=unreadable"));
        if (_gate2cSamples.Count >= 2)
        {
            var first = _gate2cSamples[0]; var last = _gate2cSamples[^1];
            var dist = MathF.Sqrt(MathF.Pow(last.x - first.x, 2) + MathF.Pow(last.y - first.y, 2) + MathF.Pow(last.z - first.z, 2));
            Log($"[gate2c] net displacement over pre-cancel window: {dist:F4} units (lockKind={_gate2cLockKind}). " +
                "As with gate2, this net figure alone does not establish sustained movement -- inspect the native " +
                "Gate2C log's per-invocation PRE/INJECTED/POST velocity and origin across the SteadyState phase " +
                "specifically, not just this harness-side first/last displacement.");

            // Independent entity-velocity cross-check (AbsVelocity, via
            // ReadPawnVelocity) against the native CMoveData postVel readback --
            // a separate measurement path, not a replacement for either. See
            // ReadPawnVelocity's own comment on why AbsVelocity's exact
            // semantics here are not yet independently verified the way
            // CMoveData's offsets are.
            if (first.velOk && last.velOk)
            {
                var firstSpeed = MathF.Sqrt(first.velX * first.velX + first.velY * first.velY + first.velZ * first.velZ);
                var lastSpeed = MathF.Sqrt(last.velX * last.velX + last.velY * last.velY + last.velZ * last.velZ);
                Log($"[gate2c] entity-velocity (AbsVelocity, independent of CMoveData readback): " +
                    $"first={firstSpeed:F4} u/s last={lastSpeed:F4} u/s -- cross-check this against the native " +
                    "log's postVel for the same PRE/POST phase before treating either alone as authoritative.");
            }

            // Heuristic-only collision/discontinuity flags. Never gates or
            // changes anything else in this test -- purely an annotation for
            // the reader, explicitly not a certain detector.
            for (int i = 1; i < _gate2cSamples.Count; i++)
            {
                var a = _gate2cSamples[i - 1];
                var b = _gate2cSamples[i];
                if (!a.velOk || !b.velOk) continue;
                bool nearReversalTick = _gate2cReverseAtMs > 0 && Math.Abs(b.tMs - (_gate2cStartMs + _gate2cReverseAtMs)) < 100;
                var speedA = MathF.Sqrt(a.velX * a.velX + a.velY * a.velY + a.velZ * a.velZ);
                var speedB = MathF.Sqrt(b.velX * b.velX + b.velY * b.velY + b.velZ * b.velZ);
                if (!nearReversalTick && speedA > 20f && speedB < speedA * 0.5f)
                {
                    Log($"[heuristic] possible collision/obstruction at t={b.tMs}: entity speed dropped from " +
                        $"{speedA:F4} to {speedB:F4} u/s between consecutive samples with no intentional reversal " +
                        "nearby -- not a confirmed collision, a plausibility flag only.");
                }
                if (MathF.Abs(b.z - a.z) > 20f)
                {
                    Log($"[heuristic] possible geometry step/fall at t={b.tMs}: Z origin changed by {b.z - a.z:F4} " +
                        "units between consecutive samples -- not a confirmed cause, a plausibility flag only.");
                }
            }
        }

        if (_gate2cRoundTransitionCounterAtStart != _roundTransitionCounter)
        {
            Log($"[gate2c] WARNING: a round transition occurred during this run ({_gate2cRoundTransitionCounterAtStart} " +
                $"-> {_roundTransitionCounter} round-start/end events) -- treat this result as potentially confounded.");
        }

        Log($"[gate2c] post-cancel (still locked) sample count={_gate2cPostCancelSamples.Count}");
        foreach (var s in _gate2cPostCancelSamples)
            Log($"[gate2c] t={s.tMs} pos=({s.x:F4},{s.y:F4},{s.z:F4}) eye=(pitch={s.pitch:F4},yaw={s.yaw:F4}) " +
                (s.velOk ? $"vel=({s.velX:F4},{s.velY:F4},{s.velZ:F4})" : "vel=unreadable"));
        if (_gate2cPostCancelSamples.Count >= 2)
        {
            var steps = new List<float>();
            for (int i = 1; i < _gate2cPostCancelSamples.Count; i++)
            {
                var a = _gate2cPostCancelSamples[i - 1];
                var b = _gate2cPostCancelSamples[i];
                steps.Add(MathF.Sqrt(MathF.Pow(b.x - a.x, 2) + MathF.Pow(b.y - a.y, 2) + MathF.Pow(b.z - a.z, 2)));
            }
            Log($"[gate2c] post-cancel per-sample step size: first={steps[0]:F5} last={steps[^1]:F5} " +
                "(decreasing or near-zero is consistent with decaying residual momentum; flat-or-increasing " +
                "across this whole window is NOT and must be treated as unexplained continued motion).");
        }

        var api = BotControllerCap.Get();
        if (api is not null)
        {
            try { api.Unlock(_gate2cSlot, LockKind.All); } catch { /* diagnostic-only, fail soft */ }
            try { api.Unlock(_gate2cSlot, LockKind.Aim); } catch { /* diagnostic-only, fail soft */ }
        }
        Log($"[gate2c] unlocked slot={_gate2cSlot}.");
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

        // Emergency stop always drops a pending teleport verification, whatever
        // its slot -- it holds no lock or native state, so dropping it is safe.
        ClearTeleportVerification($"css_poc_stop {slot}");
        // Same reasoning for a pending css_poc_setaim sampling window: it holds
        // no lock or native state of its own either (Lock(Aim) is a separate,
        // independently-unlocked step below), so dropping it is safe.
        ClearAimVerification($"css_poc_stop {slot}");
        // Same reasoning for a pending css_poc_firetest: cancels any
        // outstanding injections itself (criterion H -- no held attack
        // state survives a stop); whatever lock tier it used is released by
        // the existing unconditional Unlock(All)/Unlock(Aim) calls below.
        ClearFireTest($"css_poc_stop {slot}");
        // Same for css_poc_jumptest/crouchtest -- cancel any outstanding
        // injections, lock release covered by the unconditional Unlock calls below.
        ClearJumpTest($"css_poc_stop {slot}");
        ClearCrouchTest($"css_poc_stop {slot}");

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

        // Gate 2C: separate active-state fields from css_poc_gate2's own
        // (_gate2cActive/_gate2cSlot, not _gate2Active/_gate2Slot above), since
        // a css_poc_gate2c run does not set the latter. Finalizes BOTH native
        // diagnostics it started, through their own respective single-owner
        // guards, so an emergency stop mid-Gate2C-run never leaves either
        // native side un-finalized.
        bool wasGate2cActive = _gate2cActive || _gate2cPostCancelActive;
        _gate2cActive = false; _gate2cPostCancelActive = false;
        if (wasGate2cActive || slot == _gate2cSlot)
        {
            TryFinalizeBothGate2CDiagnostics(slot, aborted: true);
        }

        // Rest-wait: no diagnostic or movement was ever started in this phase
        // (that only happens once OnTick's rest-wait block hands off to
        // StartGate2CDiagnostic), so there is nothing to finalize here -- just
        // stop the pending continuation from firing. The lock taken when the
        // wait began is released by the unconditional Unlock(All)/Unlock(Aim)
        // calls below regardless of which slot's wait this was, same as every
        // other phase this method already covers.
        if (_gate2cWaitingForRest && slot == _gate2cWaitSlot)
        {
            _gate2cWaitingForRest = false;
            Log($"[stop] aborted pending rest-wait for slot {slot}");
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

// ---- css_poc_teleport's pure decision logic, kept free of any
// CounterStrikeSharp type so it can be unit-tested without a server
// (poc/BotControllerPocHarness.Tests). Everything that touches the engine
// stays in PocHarnessPlugin above. ----

public readonly record struct TeleportRequest(int Slot, float X, float Y, float Z, float Pitch, float Yaw, float Roll);

public static class TeleportSafety
{
    // The one and only directory css_poc_teleport is enabled from: the
    // harness's install directory on the isolated test server.
    public const string IsolatedHarnessModuleDirectory =
        @"C:\CS2IsolatedBCTest\server\game\csgo\addons\counterstrikesharp\plugins\BotControllerPocHarness";

    public const int VerifyDelayTicks = 3;

    // Wall-clock bound on a pending verification, far above VerifyDelayTicks at
    // any real tick rate; only reached if OnTick stops firing (hibernation,
    // stalled server), so a pending teleport can never block indefinitely.
    public const long VerifyExpiryMs = 2000;

    // Exact, case-insensitive match against the canonical directory. Only
    // backslash separators are accepted and at most one trailing separator is
    // tolerated; anything else (substrings, relative or '.'/'..' segments,
    // forward slashes, UNC/device prefixes, surrounding whitespace) fails.
    public static bool IsCanonicalIsolatedModuleDirectory(string? moduleDirectory)
    {
        if (string.IsNullOrEmpty(moduleDirectory)) return false;
        if (moduleDirectory.IndexOf('/') >= 0) return false;
        var candidate = moduleDirectory.EndsWith('\\') ? moduleDirectory[..^1] : moduleDirectory;
        return string.Equals(candidate, IsolatedHarnessModuleDirectory, StringComparison.OrdinalIgnoreCase);
    }

    // Returns which test phase blocks a teleport, or null if none does.
    public static string? TeleportBlockReason(bool gate2Active, bool gate2PostCancelActive, bool gate3Active,
        bool gate2cWaitingForRest, bool gate2cActive, bool gate2cPostCancelActive)
    {
        if (gate2cWaitingForRest) return "a Gate2C rest-wait";
        if (gate2cActive) return "a Gate2C session";
        if (gate2cPostCancelActive) return "a Gate2C post-cancel observation";
        if (gate2Active) return "a Gate2 session";
        if (gate2PostCancelActive) return "a Gate2 post-cancel observation";
        if (gate3Active) return "a Gate3 session";
        return null;
    }

    // args: the command's arguments after the command name, i.e.
    // [slot, x, y, z, pitch, yaw, roll?]; missing entries may be null/empty.
    public static bool TryParseTeleportArgs(IReadOnlyList<string?> args, out TeleportRequest request, out string error)
    {
        request = default;
        string? Arg(int i) => i < args.Count ? args[i] : null;

        if (!int.TryParse(Arg(0), NumberStyles.Integer, CultureInfo.InvariantCulture, out var slot) || slot < 0)
        { error = "bad slot arg -- must be a non-negative integer"; return false; }

        var values = new float[5];
        string[] names = { "x", "y", "z", "pitch", "yaw" };
        for (int i = 0; i < values.Length; i++)
        {
            if (!TryParseFinite(Arg(i + 1), out values[i]))
            { error = $"bad {names[i]} arg -- all of x/y/z/pitch/yaw must be finite numbers"; return false; }
        }

        float roll = 0f;
        var rollArg = Arg(6);
        if (!string.IsNullOrWhiteSpace(rollArg) && !TryParseFinite(rollArg, out roll))
        { error = "bad roll arg -- must be a finite number (omit for default 0.0)"; return false; }

        request = new TeleportRequest(slot, values[0], values[1], values[2], values[3], values[4], roll);
        error = string.Empty;
        return true;
    }

    // internal rather than private: AimSafety below reuses this exact check
    // rather than duplicating it.
    internal static bool TryParseFinite(string? s, out float value) =>
        float.TryParse(s, NumberStyles.Float, CultureInfo.InvariantCulture, out value) && float.IsFinite(value);

    // observed - requested, wrapped to [-180, 180).
    public static float WrapAngleDelta(float observed, float requested)
    {
        var d = (observed - requested) % 360f;
        if (d >= 180f) d -= 360f;
        else if (d < -180f) d += 360f;
        return d;
    }

    public static bool IsVerificationExpired(long nowMs, long expiresAtMs) => nowMs >= expiresAtMs;
}

// ---- css_poc_setaim's pure decision logic, kept free of any
// CounterStrikeSharp type for the same reason as TeleportSafety above
// (poc/BotControllerPocHarness.Tests). ----

public readonly record struct AimRequest(int Slot, float Pitch, float Yaw);

public static class AimSafety
{
    // The universal Source-engine look-up/down range -- a true engine
    // constant, not a guessed map-specific value (unlike teleport's
    // deliberately-unbounded x/y/z, which this harness has no basis to
    // bound since it makes no claim about any specific map). Yaw is left
    // unconstrained here: the native side already wraps it via NormalizeDeg
    // before writing.
    public const float MinPitchDeg = -89f;
    public const float MaxPitchDeg = 89f;

    // args: [slot, pitch, yaw].
    public static bool TryParseSetAimArgs(IReadOnlyList<string?> args, out AimRequest request, out string error)
    {
        request = default;
        string? Arg(int i) => i < args.Count ? args[i] : null;

        if (!int.TryParse(Arg(0), NumberStyles.Integer, CultureInfo.InvariantCulture, out var slot) || slot < 0)
        { error = "bad slot arg -- must be a non-negative integer"; return false; }

        if (!TeleportSafety.TryParseFinite(Arg(1), out var pitch))
        { error = "bad pitch arg -- must be a finite number"; return false; }
        if (pitch < MinPitchDeg || pitch > MaxPitchDeg)
        { error = $"bad pitch arg -- must be within [{MinPitchDeg},{MaxPitchDeg}] (the Source-engine look-up/down range)"; return false; }

        if (!TeleportSafety.TryParseFinite(Arg(2), out var yaw))
        { error = "bad yaw arg -- must be a finite number"; return false; }

        request = new AimRequest(slot, pitch, yaw);
        error = string.Empty;
        return true;
    }
}

// ---- css_poc_firetest's pure decision logic, kept free of any
// CounterStrikeSharp type AND free of BotControllerApi's LockKind (hence
// the local FireLockMode enum below) for the same testability reason as
// TeleportSafety/AimSafety above (poc/BotControllerPocHarness.Tests). ----

public enum FireLockMode { None, Aim, All }

public readonly record struct FireTestRequest(int Slot, int PressCount, int PressDurationMs, int GapMs, FireLockMode LockMode, int WeaponDefIndex);

public static class FireSafety
{
    public const int DefaultPressCount = 3;
    public const int DefaultPressDurationMs = 50;
    public const int DefaultGapMs = 200;
    public const int DefaultWeaponDefIndex = 4; // Glock-18: universal, infinite-buy, simple semi-auto

    // Bit 0 -- the Source-engine primary-attack input. This codebase's own
    // Gate2BDiagnostics.h (src/features/recorder/Gate2BDiagnostics.h) documents
    // a "(1<<0)|(1<<11)" mask as already independently verified "grenade-
    // related" via this exact InjectUsercmd path -- a grenade throw is
    // triggered by the primary-attack input, corroborating (not merely
    // assuming) that bit 0 functions as attack here. Every OTHER bit this
    // codebase has independently verified (kInForward=1<<3, kInBack=1<<4,
    // kInMoveLeft=1<<9, kInMoveRight=1<<10, see InputInjector.cpp) matches
    // the standard, stable Source-engine button layout exactly, the same
    // convention bit 0 (IN_ATTACK) and bit 11 (IN_ATTACK2) are drawn from.
    public const ulong AttackButtonMask = 1UL << 0;

    // args: [slot, pressCount?, pressDurationMs?, gapMs?, lockMode?, weaponDefIndex?].
    // Every optional arg falls back to its default when omitted/blank, never
    // when present-but-invalid -- a typo must be rejected, not silently
    // replaced.
    public static bool TryParseFireTestArgs(IReadOnlyList<string?> args, out FireTestRequest request, out string error)
    {
        request = default;
        string? Arg(int i) => i < args.Count ? args[i] : null;

        if (!int.TryParse(Arg(0), NumberStyles.Integer, CultureInfo.InvariantCulture, out var slot) || slot < 0)
        { error = "bad slot arg -- must be a non-negative integer"; return false; }

        int pressCount = DefaultPressCount;
        var pressCountArg = Arg(1);
        if (!string.IsNullOrWhiteSpace(pressCountArg) &&
            (!int.TryParse(pressCountArg, NumberStyles.Integer, CultureInfo.InvariantCulture, out pressCount) || pressCount < 1))
        { error = "bad pressCount arg -- must be a positive integer (omit for default 3)"; return false; }

        int pressDurationMs = DefaultPressDurationMs;
        var pdArg = Arg(2);
        if (!string.IsNullOrWhiteSpace(pdArg) &&
            (!int.TryParse(pdArg, NumberStyles.Integer, CultureInfo.InvariantCulture, out pressDurationMs) || pressDurationMs < 0))
        { error = "bad pressDurationMs arg -- must be a non-negative integer (omit for default 50; 0 means a single-tick press, see InputInjector.cpp's PendingPress->PendingRelease transition, NOT no press)"; return false; }

        int gapMs = DefaultGapMs;
        var gapArg = Arg(3);
        if (!string.IsNullOrWhiteSpace(gapArg) &&
            (!int.TryParse(gapArg, NumberStyles.Integer, CultureInfo.InvariantCulture, out gapMs) || gapMs < 0))
        { error = "bad gapMs arg -- must be a non-negative integer (omit for default 200)"; return false; }

        FireLockMode lockMode = FireLockMode.None;
        var lockModeArg = Arg(4);
        if (!string.IsNullOrWhiteSpace(lockModeArg) && !lockModeArg.Equals("none", StringComparison.OrdinalIgnoreCase))
        {
            if (lockModeArg.Equals("aim", StringComparison.OrdinalIgnoreCase)) lockMode = FireLockMode.Aim;
            else if (lockModeArg.Equals("all", StringComparison.OrdinalIgnoreCase)) lockMode = FireLockMode.All;
            else { error = $"bad lockMode arg '{lockModeArg}' -- must be none|aim|all (omit for default none)"; return false; }
        }

        int weaponDefIndex = DefaultWeaponDefIndex;
        var wdArg = Arg(5);
        if (!string.IsNullOrWhiteSpace(wdArg) &&
            (!int.TryParse(wdArg, NumberStyles.Integer, CultureInfo.InvariantCulture, out weaponDefIndex) || weaponDefIndex < 0))
        { error = "bad weaponDefIndex arg -- must be a non-negative integer (omit for default 4, Glock-18)"; return false; }

        request = new FireTestRequest(slot, pressCount, pressDurationMs, gapMs, lockMode, weaponDefIndex);
        error = string.Empty;
        return true;
    }
}

// ---- css_poc_jumptest/css_poc_crouchtest's pure decision logic, same
// design as FireSafety: free of CounterStrikeSharp AND BotControllerApi
// types (reuses FireLockMode), kept testable without a server. ----

public readonly record struct JumpTestRequest(int Slot, int JumpCount, int PressDurationMs, FireLockMode LockMode);
public readonly record struct CrouchTestRequest(int Slot, int HoldCount, int HoldMs, FireLockMode LockMode);

public static class MotorTestSafety
{
    public const int DefaultJumpCount = 2;
    public const int DefaultJumpPressDurationMs = 50;
    public const int DefaultHoldCount = 2;
    public const int DefaultHoldMs = 800;

    // Bit 1 / bit 2 -- the standard Source-engine jump/duck inputs. UNLIKE
    // the movement bits (kInForward etc, InputInjector.cpp) and the attack
    // bit (corroborated this session via live clip1 evidence, see
    // FireSafety.AttackButtonMask), these two have NOT been independently
    // verified anywhere in this codebase before now. They are a HYPOTHESIS
    // based on the standard layout, to be confirmed or refuted by actual
    // runtime engine-state evidence (LastJumpTick/velocity/height for jump,
    // Ducked/DuckAmount for crouch) -- never assumed true merely because
    // InjectUsercmd accepted the call.
    public const ulong JumpButtonMask = 1UL << 1;
    public const ulong DuckButtonMask = 1UL << 2;

    private static bool TryParseLockMode(string? arg, out FireLockMode lockMode, out string error)
    {
        lockMode = FireLockMode.None;
        error = string.Empty;
        if (string.IsNullOrWhiteSpace(arg) || arg.Equals("none", StringComparison.OrdinalIgnoreCase)) return true;
        if (arg.Equals("aim", StringComparison.OrdinalIgnoreCase)) { lockMode = FireLockMode.Aim; return true; }
        if (arg.Equals("all", StringComparison.OrdinalIgnoreCase)) { lockMode = FireLockMode.All; return true; }
        error = $"bad lockMode arg '{arg}' -- must be none|aim|all (omit for default none)";
        return false;
    }

    // args: [slot, jumpCount?, pressDurationMs?, lockMode?]
    public static bool TryParseJumpTestArgs(IReadOnlyList<string?> args, out JumpTestRequest request, out string error)
    {
        request = default;
        string? Arg(int i) => i < args.Count ? args[i] : null;

        if (!int.TryParse(Arg(0), NumberStyles.Integer, CultureInfo.InvariantCulture, out var slot) || slot < 0)
        { error = "bad slot arg -- must be a non-negative integer"; return false; }

        int jumpCount = DefaultJumpCount;
        var jcArg = Arg(1);
        if (!string.IsNullOrWhiteSpace(jcArg) &&
            (!int.TryParse(jcArg, NumberStyles.Integer, CultureInfo.InvariantCulture, out jumpCount) || jumpCount < 1))
        { error = "bad jumpCount arg -- must be a positive integer (omit for default 2)"; return false; }

        int pressDurationMs = DefaultJumpPressDurationMs;
        var pdArg = Arg(2);
        if (!string.IsNullOrWhiteSpace(pdArg) &&
            (!int.TryParse(pdArg, NumberStyles.Integer, CultureInfo.InvariantCulture, out pressDurationMs) || pressDurationMs < 0))
        { error = "bad pressDurationMs arg -- must be a non-negative integer (omit for default 50)"; return false; }

        if (!TryParseLockMode(Arg(3), out var lockMode, out error)) return false;

        request = new JumpTestRequest(slot, jumpCount, pressDurationMs, lockMode);
        error = string.Empty;
        return true;
    }

    // args: [slot, holdCount?, holdMs?, lockMode?]
    public static bool TryParseCrouchTestArgs(IReadOnlyList<string?> args, out CrouchTestRequest request, out string error)
    {
        request = default;
        string? Arg(int i) => i < args.Count ? args[i] : null;

        if (!int.TryParse(Arg(0), NumberStyles.Integer, CultureInfo.InvariantCulture, out var slot) || slot < 0)
        { error = "bad slot arg -- must be a non-negative integer"; return false; }

        int holdCount = DefaultHoldCount;
        var hcArg = Arg(1);
        if (!string.IsNullOrWhiteSpace(hcArg) &&
            (!int.TryParse(hcArg, NumberStyles.Integer, CultureInfo.InvariantCulture, out holdCount) || holdCount < 1))
        { error = "bad holdCount arg -- must be a positive integer (omit for default 2)"; return false; }

        int holdMs = DefaultHoldMs;
        var hmArg = Arg(2);
        if (!string.IsNullOrWhiteSpace(hmArg) &&
            (!int.TryParse(hmArg, NumberStyles.Integer, CultureInfo.InvariantCulture, out holdMs) || holdMs < 1))
        { error = "bad holdMs arg -- must be a positive integer (omit for default 800)"; return false; }

        if (!TryParseLockMode(Arg(3), out var lockMode, out error)) return false;

        request = new CrouchTestRequest(slot, holdCount, holdMs, lockMode);
        error = string.Empty;
        return true;
    }
}

# BotController Control-Chain PoC Harness

Disposable, experimental. Not part of `cs2-ai-training`'s production solution; not
referenced by any `.sln`; not built by any existing CI. Exists only to exercise
BotController v0.7.0's existing public control surface for Gates 1-3 of the
approved PoC. Does not vendor or copy any of BotController's (AGPL-3.0) own
source -- it references the already-built `BotControllerApi.dll` at runtime via
CounterStrikeSharp's `PluginCapability` interop, the same interface-lookup shape
CS2-Smarter-Bot already uses in production, not source inclusion.

## Files
- `BotControllerPocHarness.csproj` -- net10.0, CounterStrikeSharp.API 1.0.375, references the
  already-extracted, hash-verified `BotControllerApi.dll` from `C:\CS2BotControllerBuild`.
- `PocHarnessPlugin.cs` -- the harness itself. Console commands: `css_poc_gate1`,
  `css_poc_baseline <slot>`, `css_poc_gate2 <slot> <durationMs>`,
  `css_poc_gate3 <slot> <pitch> <yaw> <durationMs>` (**disabled**, see below),
  `css_poc_stop <slot>`.

## Build blocker -- reported, not worked around

This machine has only the .NET 8 SDK installed (`dotnet --list-sdks` -> `8.0.424`
only, confirmed this session). A `net10.0` project cannot be built by an 8.0 SDK.
This harness is therefore **prepared but not built**. Two paths exist to build it,
both requiring your separate approval (neither was taken):

1. Install the .NET 10 SDK locally -- explicitly NOT authorized by this task
   ("Do not install additional software").
2. Add a small job to the already-forked, already-proven `Jebus8five/CS2-Bot-Controller`
   GitHub Actions workflow (or a new, separate fork) that builds this harness against
   the pinned commit's `BotControllerApi.dll` the same way `mm-windows`/`css` already
   build BotController itself -- this is a new build/dispatch action, not something
   this task's "build if already available" authorization covers, since the
   dependency (.NET 10 SDK) is not already available locally.

## What this harness deliberately does NOT do
- No new native hooks; no changes to BotController's own `.cpp`/`.h` files.
- No changes to `IScenarioActorController`, `ScenarioExecutionAuthority`, or any
  other governed `cs2-ai-training` file.
- No hardcoded map coordinates or angles -- `css_poc_gate3`'s pitch/yaw are
  operator-supplied console-command arguments, never invented by this code.
- No automatic execution on load -- every gate is a deliberate, operator-issued
  console command.

## Gate 3 disabled (post-restart correction)

`css_poc_gate3` is now **disabled** (`OnGate3` logs and returns immediately; its
original body is preserved below an unreachable-code guard for the future fix,
not deleted). This replaces the previous "not established from source alone"
framing: the pinned-commit native source was read this session and the question
is now **resolved, not open**.

**Source-verified finding**, `MotionRecorder.cpp` at
`github.com/Jebus8five/CS2-Bot-Controller@9304ee6727e78e7f116b611a5c246278ace01223`:
- `StartReplay()` sets `needsInitialTeleport = true` unconditionally -- there is no
  "no position supplied" case.
- `OnReplayCommandPre()`: on the first tick, calls the native `CBaseEntity::Teleport`
  export directly with `position = tick.pre.origin{X,Y,Z}`. With that zeroed, this
  **is** a teleport-to-world-origin call, not a hypothetical one. Every tick after
  that (including the same one), `WriteSceneNodeOrigin(pawn, tick.pre)`
  unconditionally overwrites the pawn's `AbsOrigin` scene-node memory from the same
  zeroed values -- there is no field-presence bitmask gating this, unlike
  `ReplayCommandFrameData`'s `forwardMove`/`leftMove`/etc. in
  `InputInjector.cpp::ApplyReplayUserCommand`, which do use a `fields` bitmask.
- `OnReplayCommit()` repeats the same unconditional `WriteSceneNodeOrigin` from
  `tick.post`.
- The same two functions also unconditionally force-write `AbsVelocity`
  (`WriteVelocityToPawn`), `MoveType`/`ActualMoveType`, the OnGround/Ducking bits of
  `EntityFlags`, and `DuckAmount`/`DuckSpeed`/`LadderNormal`
  (`WriteMovementServiceState`) from the same zeroed snapshot, every tick.

**Conclusion:** as originally written, Gate 3 was not a clean aim-only test -- it
would teleport the bot to world origin and clobber its velocity/movetype/ground-flag/
duck state on every replay tick. Per this task's instruction to disable or defer a
gate rather than run an unverified synthetic replay, Gate 3 is disabled rather than
patched with a guessed "safe" origin value.

**What re-enabling it correctly would require:** a live pre-read of the target bot's
actual `OriginX/Y/Z`, `VelX/Y/Z`, `MoveType`, `ActualMoveType`, `EntityFlags`,
`DuckAmount`/`DuckSpeed`, and `LadderNormal`, fed into the `MovementSnapshot` so the
native forced-writes above are no-ops for everything except the operator-supplied
`Pitch`/`Yaw`. This harness currently only has C# schema read code for
`AbsOrigin`/`EyeAngles` (`ReadPawnState`) -- the rest are not yet wired up, which is
why this is a "disable" rather than a same-session "fix."

## Corrected acceptance criteria: AI interference (post-restart)

AI-suppression success/failure must be judged **only** from behavior sampled while
our `Lock`/suppression is actively held (the sampling windows in `OnTick` during
Gate 2/3). Once the operator calls `css_poc_stop` (or a gate's own cleanup releases
the lock), the bot resuming ordinary `CCSBot` AI behavior is the **correct, expected**
outcome -- it must never be scored as an interference failure or as evidence that
suppression didn't work. (This corrects an earlier framing that treated post-unlock
AI activity as something needing a follow-up "reassertion" check; that framing has
been removed from `PocHarnessPlugin.cs`'s Gate 2 comments.)

## Corrected: native hook-call counters do not exist in this build

The original harness design read four native diagnostic exports
(`BotController_GetHookCallCount`, `BotController_GetPlayerRunCommandCallCount`,
`BotController_GetPhysicsSimulateCallCount`, `BotController_GetFinishMoveCallCount`)
as before/after deltas, intended as evidence that hooked functions were actually
firing. This session verified against both the pinned-commit source
(`src/bridge/exports.cpp`) and the literal symbol strings inside the already
hash-verified `BotController.dll` (`C:\CS2BotControllerBuild\extracted-MM\...`):
**none of these four exports, nor any "CallCount" symbol at all, exist in this
build.** The DllImport declarations and Gate 1's use of them have been removed --
they would have thrown `EntryPointNotFoundException` on first call. Gate 1 now
reports capability resolution + `AbiVersion` only, and does not claim this as proof
any specific hook fires for any specific bot. The only real evidence of control in
this harness remains Gate 2/3's own independent schema read-back of the target
pawn's `AbsOrigin`/`EyeAngles` -- never BotController's own boolean return values,
and (per the above) never a native call counter, because none exists here.

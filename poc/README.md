# poc/

Disposable, external proof-of-concept material vendored into this fork **only**
so GitHub Actions can build it with the .NET 10 SDK, which was unavailable on
the requesting machine. Nothing under this directory is part of
`Jebus8five/CS2-Bot-Controller`'s own production surface:

- Not referenced by `CMakeLists.txt`, any existing `.sln`, or any existing
  `.csproj` outside this directory.
- Not built by the existing `.github/workflows/build.yml` -- built only by the
  separate, additive `.github/workflows/poc-harness-build.yml`.
- Does not modify BotController's own native (`src/`) or managed
  (`csharp/BotControllerImpl`, `csharp/BotControllerApi`) implementation.
  `poc/BotControllerPocHarness` references `csharp/BotControllerApi` read-only,
  via a `ProjectReference`, to pick up the interface assembly it needs to build
  against.

See `poc/BotControllerPocHarness/README.md` for what the harness itself does,
its disabled Gate 3, and its corrected acceptance criteria. It originates from
the `cs2-ai-training` project's own experimental investigation, not from this
repository.

# m5tools -- M5 Pro Hardware Monitoring Daemons

Three tools for M5 Pro hardware monitoring and control: a fan daemon, a
telemetry logger, and an interactive terminal monitor. Used by the hardware
monitoring LaunchAgents on the captain's MacBook Pro.

## Stack

- **m5fand** -- C99, `clang`. Root daemon. Polls SMC for CPU/GPU die temps,
  applies a sigmoid fan curve with hysteresis, writes target RPMs via SMC.
  Logs to `~/Library/Logs/m5fand.log`. Depends on `~/.local/bin/smc` (external
  binary, not in this repo).
- **m5logd** -- C99, `clang`, links `-lm`. User daemon. Binary delta log of CPU
  cluster utilization (P/E cluster via sysctl), memory pressure (mach VM stats
  + swap), and thermal pressure (sysctl). Only records deltas -- ~100
  records/day at idle vs 43k for text logs. Output: `~/Library/Logs/m5logd.m5l`
  (M5LOGv1 binary format). Reader tool: `m5log` (separate tool, not in this
  repo).
- **m5mon** -- Go 1.26.4, stdlib only. Interactive terminal monitor (TUI) with
  two-second refresh: per-core temps, fan speeds, memory, load, battery, top
  processes, zombie/stuck detection. Supports `-json` flag for one-shot JSON
  output. Not a daemon -- it is run interactively.
- **smc** -- External binary at `~/.local/bin/smc`. All three tools shell out
  to it for SMC reads/writes. Not included in this repo.

## LaunchAgents

Two LaunchAgent plists in `launchd/`:

| Plist | Daemon | User | Type |
|---|---|---|---|
| `com.jwalinshah.m5fand.plist` | m5fand | root | `RunAtLoad` + `KeepAlive` |
| `com.jwalinshah.m5logd.plist` | m5logd | jwalinshah (user) | `RunAtLoad` + `KeepAlive` |

m5mon has no LaunchAgent -- it is an interactive TUI, not a persistent daemon.

The plists are deployed via Nix (`configuration.nix` in dotfiles). They are
checked into this repo as the authoritative copy; dotfiles references them.

## Build

```sh
make           # builds all three: m5fand, m5logd, m5mon
make -C m5fand # build just one
```

Each daemon has its own Makefile. `make install` copies binaries to
`~/.local/bin/` and codesigns (`codesign --sign - --force`).

## Test

```sh
make test
```

Tests are minimal: m5fand and m5logd only verify the binary exists and is
executable. m5mon runs `go vet ./...` plus the existence check. There are no
unit tests, integration tests, or CI.

## Sharp edges

- **smc binary is external.** All three tools shell out to `~/.local/bin/smc`.
  If `smc` is not installed or not codesigned, m5fand will silently fail to
  control fans and m5mon will show zeroes. m5mon falls back to `$PATH` lookup
  if the hardcoded path is missing, but m5fand and m5logd do not.
- **m5fand runs as root** (see LaunchAgent `UserName` key). It needs root to
  write SMC keys. The `smc` binary must also be root-executable.
- **m5fand hardcodes paths.** SMC binary path is a `#define` at compile time.
  Log path uses `$HOME` with a hardcoded fallback. Both need updating if paths
  change.
- **m5logd binary format.** The `.m5l` format is custom (M5LOGv1 header, delta-
  encoded records with type tags). The `m5log` reader tool is a separate
  project -- not in this repo. If `m5log` breaks, telemetry data is
  inaccessible.
- **No unit tests.** The C daemons have no test coverage. m5mon has only `go
  vet`. Changes to fan curve logic or thermal reading must be manually verified
  on real hardware.
- **m5mon shells out per refresh.** Every 2-second cycle, m5mon calls `smc -l`,
  `vm_stat`, `sysctl`, `ps`, `pmset`, `system_profiler`, and `ioreg`. At idle
  this is fine; under heavy load the shell-out overhead is non-trivial. The
  JSON mode (`-json`) is a single one-shot report, not a streaming mode.
- **m5fand fan curve is compile-time.** The 30-point sigmoid curve (temp,
  target RPM) is a static C array. Adjusting the curve requires recompilation
  and reinstallation.
- **No m5mon LaunchAgent.** m5mon is not a daemon -- there is no plist for it.
  The `launchd/` directory only contains m5fand and m5logd.

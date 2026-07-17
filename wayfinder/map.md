# m5tools — Wayfinder Map

Label: `wayfinder:map`
Created: 2026-07-17

## What It Is

C + Go hardware monitoring daemons for Apple Silicon Macs:
- **m5fand** — fan control daemon (root), reads SMC temps, applies fan curve
- **m5logd** — hardware logging daemon, delta-encoded binary telemetry
- **m5mon** — interactive thermal TUI

## Current State

- **Branch:** `main` — clean
- **Build:** ✅ make test passes (minimal tests — verify binaries exist)
- **Deployed:** Yes — both daemons running (m5fand as root, m5logd as user)
- **Last commit:** `8cddab4` (2026-07-16) add AGENTS.md

## Known sharp edges

- External `smc` binary — all tools shell out to `~/.local/bin/smc`
- No unit tests for C daemons — fan curve logic must be manually verified
- Compile-time fan curve — adjusting requires recompilation and reinstall
- Shell-out overhead in m5mon — 7+ external commands every 2-second cycle

## Tickets

### 🔴 Active

1. **Runtime-configurable fan curve** — currently a compile-time `#define`. Read from a config file instead.

### 🟡 Next

2. **Add unit tests for C daemons** — temp reading, fan curve calculation, log delta encoding.

3. **Reduce m5mon shell-out overhead** — batch SMC queries or use libsmc directly.

### 🔵 Future

4. **m5log reader** — the `m5log` binary that reads M5LOGv1 binary format is a separate project not in this repo. Bring it in or document where it lives.

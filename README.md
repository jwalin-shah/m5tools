# m5tools

> One-line system description coming soon. Part of the jw-* agent orchestration ecosystem.

## Status

This repository is actively maintained. Documentation is being consolidated into the [dotfiles](https://github.com/jwalin-shah/dotfiles) monorepo and [jw-core](https://github.com/jwalin-shah/jw-core) orchestrator.

See [ARCHITECTURE.md](https://github.com/jwalin-shah/dotfiles) for the full system architecture.

## Build

```bash
# Build instructions vary by repo. Check the Makefile or go.mod.
make build 2>/dev/null || go build ./...
```

## Ecosystem

Part of the jw-* agent orchestration system alongside [jw-core](https://github.com/jwalin-shah/jw-core), [jw-sentry](https://github.com/jwalin-shah/jw-sentry), [jw-sessiond](https://github.com/jwalin-shah/jw-sessiond), and other jw-* daemons.


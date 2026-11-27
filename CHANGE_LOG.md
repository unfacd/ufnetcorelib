# Change Log — ufnetcorelib

## Release-261127 — 2.5.0
`af8ab20f` → `7ac76d45` · 2017-06-16 → 2026-11-27

First mirror: the tree as of v2.5.0. There is no previous release to diff against, so this
entry describes what the library provides rather than a delta.

### New
- **Session lifecycle and I/O** — Moved off to ufsrvcorelib, leaving thin unfacd focused semantics
- **protobuf** — Now owns the schemas and `protobuf-c` 1.4.1 is vendored for the generator

### Fixes

### Deprecated
- (none)

### Removed
- The legacy rate-limit implementation, retired in favour of `ratelimit_v2`

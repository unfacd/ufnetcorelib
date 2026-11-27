#!/usr/bin/env bash
# dev/scripts/dev_build.sh — configure (+ optional compose, + optional tests) → build.
#
# Fast development loop: configure the repo with the chosen CMake preset and
# build it.  No install, verify, or push — this is the inner build-only loop.
#
# Usage:
#   dev_build.sh [PRESET] [--fresh] [--clean] [--clean-compose] [--compose]
#                [--test] [--jobs N] [--dry-run] [--help]
#
# PRESET (default: dev) selects the CMake preset and names build/<PRESET>
# (or build/<PRESET>-test when --test is given, so a tests-ON build never
# disturbs the tests-OFF build directory).
#   --fresh          fresh configure (cmake --fresh): full clean rebuild
#   --clean          run the build's 'clean' target first (object files only)
#   --clean-compose  run ufnetcorelib_ufsrv_config_clean, then recompose —
#                    removes the composition litter (staging, composer scratch,
#                    copied toolchain files, invariant/shim copies) and restores
#                    it.  IMPLIES --compose; the pair is inseparable, see below
#   --compose        run the ufsrv_config composition (ufsrv_config_compose_host)
#                    once, before the build.  Requires a self-test preset
#   --test           enable gtest (-D_PACKAGE_TESTS=ON) in build/<PRESET>-test
#
# --clean and --clean-compose are independent.  Both need an already-configured
# build dir; on one that has never been configured they print 'skipped' rather
# than failing.
#
# Composition is explicit and dormant: nothing else in the build regenerates
# the joined deep tree, so --compose is the only thing that does.  It needs a
# preset that sets UFSRV_CONFIG_MODULE_UFNETCORELIB=ON, i.e.
# dev-ufnetcorelib-ufsrvconfig:
#
#   dev_build.sh dev-ufnetcorelib-ufsrvconfig --compose
#
# Run it once after a catalogue change; plain builds afterwards are enough.
#
# Exit status: 0 on success; non-zero if any stage fails (set -euo pipefail).
set -euo pipefail

# ── Defaults ────────────────────────────────────────────────────────────────

PRESET="dev"
JOBS="$(nproc)"
FRESH=0
RUN_TESTS=0
RUN_COMPOSE=0
CLEAN_BUILD=0
CLEAN_COMPOSE=0
DRY_RUN=0

usage() {
    cat <<EOF
Usage: $0 [PRESET] [options]

Configure and build (no install / verify / push).

  $0 dev                                      # configure + build the 'dev' preset
  $0 dev --test                               # same, with gtest (-D_PACKAGE_TESTS=ON)
  $0 dev-ufnetcorelib-ufsrvconfig --compose   # self-test: compose the joined tree, then build

Options:
  --preset NAME  Explicit preset (same as the positional PRESET)
  --jobs N       Parallel build jobs (default: \$(nproc))
  --fresh        Fresh configure (cmake --fresh): full clean rebuild
  --clean        Run the build's 'clean' target and exit — object files only
  --clean-compose  Run ufnetcorelib_ufsrv_config_clean and exit: removes staging,
                 composer scratch, copied toolchain files, the invariant/shim
                 copies and .composed.  Preserves the tracked generated/ output
  --compose      Run ufsrv_config_compose_host before building.  Requires a
                 preset with UFSRV_CONFIG_MODULE_UFNETCORELIB=ON, e.g.
                 dev-ufnetcorelib-ufsrvconfig.  Composition is explicit and
                 dormant; this is the only thing that regenerates the deep tree
  --test         Enable gtest (-D_PACKAGE_TESTS=ON) in build/<preset>-test
  --dry-run      Print commands without executing them
  -h, --help     Show this help

--clean and --clean-compose are TERMINAL: they clean and exit, without
configuring, composing or building.  Neither is gated on the preset — whether it
can compose has no bearing on whether the litter is removed.  Run the script
again without a clean flag to build.
EOF
}

# ── Argument parsing ────────────────────────────────────────────────────────

while [ $# -gt 0 ]; do
    case "$1" in
        --preset)  PRESET="$2"; shift 2 ;;
        --jobs)    JOBS="$2"; shift 2 ;;
        --fresh)   FRESH=1; shift ;;
        --clean)   CLEAN_BUILD=1; shift ;;
        --clean-compose) CLEAN_COMPOSE=1; shift ;;
        --compose) RUN_COMPOSE=1; shift ;;
        --test)    RUN_TESTS=1; shift ;;
        --dry-run) DRY_RUN=1; shift ;;
        -h|--help) usage; exit 0 ;;
        --*)       echo "error: unknown option '$1'" >&2; usage >&2; exit 2 ;;
        *)         PRESET="$1"; shift ;;
    esac
done

if [ "$RUN_TESTS" -eq 1 ]; then
    BUILD_DIR="build/${PRESET}-test"
else
    BUILD_DIR="build/${PRESET}"
fi

# ── Helpers ─────────────────────────────────────────────────────────────────

step() { printf '\n==> %s\n' "$*"; }
run() {
    printf '+ %s\n' "$*"
    if [ "$DRY_RUN" -eq 0 ]; then
        "$@"
    fi
}

printf '==> dev_build: preset=%s build=%s tests=%s clean=%s clean-compose=%s compose=%s fresh=%s jobs=%s\n' \
    "$PRESET" "$BUILD_DIR" \
    "$([ "$RUN_TESTS" -eq 1 ] && echo ON || echo OFF)" \
    "$([ "$CLEAN_BUILD" -eq 1 ] && echo yes || echo no)" \
    "$([ "$CLEAN_COMPOSE" -eq 1 ] && echo yes || echo no)" \
    "$([ "$RUN_COMPOSE" -eq 1 ] && echo yes || echo no)" \
    "$([ "$FRESH" -eq 1 ] && echo yes || echo no)" \
    "$JOBS"

STAGE_N=0
stage() {
    STAGE_N=$((STAGE_N + 1))
    printf '\n==> [%d/%d] %s\n' "$STAGE_N" "$STAGE_TOTAL" "$*"
}

# A clean target needs a configured tree.  Under --dry-run we cannot tell, so
# the command is printed anyway.
needs_configure() {
    if [ "$DRY_RUN" -eq 0 ] && [ ! -f "$BUILD_DIR/CMakeCache.txt" ]; then
        printf '    skipped: %s is not configured yet\n' "$BUILD_DIR"
        return 1
    fi
    return 0
}

# ── Cleans are terminal, and unconditional ──────────────────────────────────
# --clean and --clean-compose perform their removals and exit.  They do not
# configure, compose or build, and nothing about the preset gates them: whether
# it can compose has no bearing on whether the litter gets removed.

if [ "$CLEAN_BUILD" -eq 1 ] || [ "$CLEAN_COMPOSE" -eq 1 ]; then
    STAGE_TOTAL=0
    if [ "$CLEAN_BUILD" -eq 1 ];   then STAGE_TOTAL=$((STAGE_TOTAL + 1)); fi
    if [ "$CLEAN_COMPOSE" -eq 1 ]; then STAGE_TOTAL=$((STAGE_TOTAL + 1)); fi

    if [ "$CLEAN_BUILD" -eq 1 ]; then
        stage "Clean build objects ($BUILD_DIR)"
        if needs_configure; then
            run cmake --build "$BUILD_DIR" --target clean
        fi
    fi

    if [ "$CLEAN_COMPOSE" -eq 1 ]; then
        stage "Clean composition litter (ufnetcorelib_ufsrv_config_clean)"
        if needs_configure; then
            run cmake --build "$BUILD_DIR" --target ufnetcorelib_ufsrv_config_clean
        fi
    fi

    if [ "$RUN_COMPOSE" -eq 1 ] || [ "$RUN_TESTS" -eq 1 ] || [ "$FRESH" -eq 1 ]; then
        printf '\nnote: cleans are terminal — --compose/--test/--fresh were not applied.\n' >&2
        printf '      re-run without a clean flag to build.\n' >&2
    fi
    if [ "$CLEAN_COMPOSE" -eq 1 ]; then
        printf '\nnote: the deep-tree invariant copies and shim are now absent.\n' >&2
        printf '      a self-test build needs them back:  %s dev-ufnetcorelib-ufsrvconfig --compose\n' "$0" >&2
    fi

    printf '\n==> done\n'
    exit 0
fi

# ── Configure ───────────────────────────────────────────────────────────────

STAGE_TOTAL=2
if [ "$RUN_COMPOSE" -eq 1 ]; then
    STAGE_TOTAL=3
fi

stage "Configure (preset=$PRESET, build=$BUILD_DIR)"
configure_args=(--preset "$PRESET" -B "$BUILD_DIR")
if [ "$FRESH" -eq 1 ]; then
    configure_args=(--fresh "${configure_args[@]}")
fi
if [ "$RUN_TESTS" -eq 1 ]; then
    configure_args+=(-D_PACKAGE_TESTS=ON)
fi
run cmake "${configure_args[@]}"

# ── Compose (optional) ──────────────────────────────────────────────────────
# The composition pipeline is explicit and dormant: it is not an edge of any
# other target, so a build never triggers it.  Running it here is the one thing
# that regenerates the joined deep tree.

if [ "$RUN_COMPOSE" -eq 1 ]; then
    stage "Compose (ufsrv_config_compose_host)"

    if [ "$DRY_RUN" -eq 0 ] &&
       ! grep -q '^UFSRV_CONFIG_MODULE_UFNETCORELIB:BOOL=ON' "$BUILD_DIR/CMakeCache.txt" 2>/dev/null; then
        printf 'error: --compose needs a preset with UFSRV_CONFIG_MODULE_UFNETCORELIB=ON;\n' >&2
        printf '       %s does not have it, so it has no ufsrv_config_compose_host target.\n' \
               "$BUILD_DIR" >&2
        printf '       try: %s dev-ufnetcorelib-ufsrvconfig --compose\n' "$0" >&2
        exit 2
    fi

    run cmake --build "$BUILD_DIR" --target ufsrv_config_compose_host
fi

# ── Build ───────────────────────────────────────────────────────────────────

stage "Build ($JOBS jobs)"
run cmake --build "$BUILD_DIR" -j"$JOBS"

printf '\n==> done\n'

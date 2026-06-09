#!/usr/bin/env bash
# One-step build for EloqKV.
#
# On first run this script:
#   1. Locates or auto-clones eloq_build_env (the shared build environment)
#   2. Builds all shared dependencies  (~30-60 min; cached on subsequent runs)
#   3. Builds the shared core (data_substrate)
#   4. Builds EloqKV
#
# Subsequent runs skip steps 2-3 automatically (completion markers in install/.eloqdb/).
#
# eloq_build_env resolution order:
#   1. ELOQ_BUILD_ENV env var   export ELOQ_BUILD_ENV=/path/to/eloq_build_env
#   2. ./eloq_env symlink       ln -s /path/to/eloq_build_env ./eloq_env  (add to .gitignore)
#   3. ../eloq_build_env        auto-cloned as a sibling on first run if not present
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
_SIBLING="$(dirname "$SCRIPT_DIR")/eloq_build_env"
_REPO="${ELOQ_BUILD_ENV_REPO:-https://github.com/ltzhang/eloq_build_env.git}"

_log() { printf '\033[1;34m[eloqkv]\033[0m %s\n' "$*" >&2; }

_resolve_build_env() {
    if [ -n "${ELOQ_BUILD_ENV:-}" ]; then
        [ -d "$ELOQ_BUILD_ENV" ] || { printf 'error: ELOQ_BUILD_ENV="%s" does not exist\n' "$ELOQ_BUILD_ENV" >&2; exit 1; }
        cd "$ELOQ_BUILD_ENV" && pwd; return
    fi
    if [ -L "$SCRIPT_DIR/eloq_env" ]; then
        cd "$SCRIPT_DIR/eloq_env" && pwd; return
    fi
    if [ -d "$_SIBLING" ]; then
        cd "$_SIBLING" && pwd; return
    fi
    _log "eloq_build_env not found — cloning to $_SIBLING ..."
    git clone "$_REPO" "$_SIBLING"
    cd "$_SIBLING" && pwd
}

BUILD_ENV="$(_resolve_build_env)"
source "$BUILD_ENV/env.sh"

_dep_done() { [ -f "$ELOQDB_PREFIX/.eloqdb/$1.done" ]; }

if ! _dep_done braft; then
    _log "building shared dependencies (first run — this may take 30-60 min) ..."
    bash "$ELOQDB_ROOT/scripts/deps.sh"
fi

if ! _dep_done data_substrate; then
    _log "building shared core (data_substrate) ..."
    ELOQDB_ELOQ_MODULE=ELOQ_MODULE_ELOQKV bash "$ELOQDB_ROOT/scripts/substrate.sh"
fi

export ELOQKV_DIR="$SCRIPT_DIR"
exec bash "$ELOQDB_ROOT/scripts/projects/eloqkv.sh"

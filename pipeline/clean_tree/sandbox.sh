#!/usr/bin/env bash
# A shell inside a bubblewrap sandbox that sees only the clean tree (read-write), the
# datasets and third-party DuckDB inputs (read-only), its own engine-state directory and
# the Docker socket (the Postgres/MySQL hosts are containers). Bound paths keep their real
# paths, because the Docker daemon outside the sandbox mounts the tree and the state dir.
# /home, /flash1 and every other checkout stay invisible.
#
# usage: sandbox.sh <clean-tree> [command...]          (default: an interactive bash)
#
#   ACCEL_SANDBOX_DATA      read-only data dirs/files, ':'-separated (datasets, DuckDB DBs)
#   ACCEL_DUCKDB_ROOT       the DuckDB source/build the backends compile against (read-only)
#   ACCEL_DUCKDB_SEED_DIRS  optional prebuilt in-container DuckDB builds, ':'-separated (read-only)
#   ACCEL_SANDBOX_STATE     engine-state root, read-write and at its real path so the Docker
#                           daemon can mount it (default /flash1/$USER/engine_state). A run's
#                           ACCEL_ENGINE_STATE_DIR goes below it: its gates' instances are <dir>-<name>
#                           beside it, and everything else under /flash1 is a tmpfs (memory)
#   ACCEL_HOST_LOCK_DIR     host-wide core-slot and measurement locks, read-write and at its real path so that
#                           concurrent sandboxes (one per tree) share one partition (the sandbox's /dev/shm is private)
#   OPENROUTER_API_KEY      passed through for codex when set (else <clean-tree>/.env has it)
set -euo pipefail
# Every run's memory cap (run_outcome.py capped) is a child of a cgroup the sandbox may write: systemd cannot make
# scopes for a process in its pid namespace. The sandbox runs in a delegated user scope and gets that cgroup.
if [[ -z "${ACCEL_SANDBOX_SCOPED:-}" ]] && systemd-run --user --scope -q --collect true 2>/dev/null; then
  exec env ACCEL_SANDBOX_SCOPED=1 systemd-run --user --scope -q --collect -p Delegate=yes -- "$0" "$@"
fi
cgroup=()
if [[ -n "${ACCEL_SANDBOX_SCOPED:-}" ]]; then
  root="/sys/fs/cgroup$(cut -d: -f3 /proc/self/cgroup)"
  mkdir -p "${root}/main" && echo $$ >"${root}/main/cgroup.procs" && echo +memory >"${root}/cgroup.subtree_control" &&
    cgroup=(--bind "${root}" "${root}" --setenv ACCEL_CGROUP "${root}")
fi
TREE="$(realpath "${1:?usage: sandbox.sh <clean-tree> [command...]}")"; shift
STATE="${ACCEL_SANDBOX_STATE:-/flash1/$(id -un)/engine_state}"
if [[ -n "${ACCEL_ENGINE_STATE_DIR:-}" && "${ACCEL_ENGINE_STATE_DIR}" != "${STATE}"/?* ]]; then
  echo "sandbox.sh: ACCEL_ENGINE_STATE_DIR (${ACCEL_ENGINE_STATE_DIR}) must be below ACCEL_SANDBOX_STATE (${STATE})" >&2
  exit 2
fi
mkdir -p "${STATE}" "${TREE}/.home"
CODEX_BIN="$(readlink -f "$(command -v codex)")"   # the codex CLI and its package (read-only)
CODEX_PKG="$(dirname "$(dirname "${CODEX_BIN}")")"

# Secrets pass through only when set; otherwise the tree's .env supplies them.
passenv=()
[[ -z "${OPENROUTER_API_KEY:-}" ]] || passenv+=(--setenv OPENROUTER_API_KEY "${OPENROUTER_API_KEY}")

lock=()
if [[ -n "${ACCEL_HOST_LOCK_DIR:-}" ]]; then
  mkdir -p "${ACCEL_HOST_LOCK_DIR}"
  lock=(--bind "${ACCEL_HOST_LOCK_DIR}" "${ACCEL_HOST_LOCK_DIR}" --setenv ACCEL_HOST_LOCK_DIR "${ACCEL_HOST_LOCK_DIR}")
fi

ro=()
IFS=: read -ra data <<<"${ACCEL_SANDBOX_DATA:-}:${ACCEL_DUCKDB_ROOT:-}:${ACCEL_DUCKDB_SEED_DIRS:-}"
for p in "${data[@]}"; do [[ -n "${p}" ]] && ro+=(--ro-bind "${p}" "${p}"); done

exec bwrap --die-with-parent --unshare-pid --unshare-ipc --unshare-uts \
  --ro-bind /usr /usr --symlink usr/lib /lib --symlink usr/lib /lib64 \
  --symlink usr/bin /bin --symlink usr/bin /sbin --ro-bind /etc /etc \
  --dev /dev --proc /proc --ro-bind /sys /sys --tmpfs /tmp --tmpfs /dev/shm --tmpfs /home --tmpfs /flash1 \
  --bind "${TREE}" "${TREE}" --bind "${STATE}" "${STATE}" \
  --ro-bind "${CODEX_PKG}" "${CODEX_PKG}" --symlink "${CODEX_BIN}" /opt/bin/codex \
  --bind /var/run/docker.sock /var/run/docker.sock \
  "${ro[@]}" \
  --clearenv \
  --setenv PATH "/opt/bin:/usr/bin" --setenv HOME "${TREE}/.home" --setenv USER "$(id -un)" \
  --setenv TERM "${TERM:-xterm}" --setenv TMPDIR /tmp \
  --setenv ACCEL_DUCKDB_ROOT "${ACCEL_DUCKDB_ROOT:-}" \
  "${passenv[@]}" "${lock[@]}" "${cgroup[@]}" \
  --setenv ACCEL_TREE "${TREE}" \
  --chdir "${TREE}" \
  "${@:-bash}"

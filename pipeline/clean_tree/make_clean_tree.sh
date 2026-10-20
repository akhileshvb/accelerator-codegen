#!/usr/bin/env bash
# Materialise the clean tree: <src-repo> at <rev> (default HEAD), exported with `git archive`
# (no history, no link back to the source), pruned to the manifest's KEEP set, the skeleton
# overlaid, and committed to a fresh repository (one commit: the baseline that the primitives
# additions-only gate diffs against).
#
# usage: make_clean_tree.sh <src-repo> <dest-dir> [rev]      (dest-dir must not exist)
set -euo pipefail
SRC="${1:?usage: make_clean_tree.sh <src-repo> <dest-dir> [rev]}"
DEST="${2:?usage: make_clean_tree.sh <src-repo> <dest-dir> [rev]}"
REV="${3:-HEAD}"
[[ -e "${DEST}" ]] && { echo "${DEST} exists" >&2; exit 1; }

COMMIT="$(git -C "${SRC}" rev-parse --short "${REV}")"
mkdir -p "${DEST}"
DEST="$(cd "${DEST}" && pwd)"
git -C "${SRC}" archive "${REV}" | tar -x -C "${DEST}"

python3 "${DEST}/pipeline/clean_tree/manifest.py" prune "${DEST}"

cd "${DEST}"
git init -q
git add -A
git -c user.name="clean tree" -c user.email="clean@localhost" \
  commit -q -m "Clean tree: substrate and scaffolding (exported from ${COMMIT})"
echo "clean tree: ${DEST} ($(git ls-files | wc -l) files, from ${COMMIT})"

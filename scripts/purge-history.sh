#!/usr/bin/env bash
#
# purge-history.sh — DESTRUCTIVE git history rewrite.
#
# Permanently scrubs a set of leaked / off-brand internal files from the
# ENTIRE git history of this repository (every commit, every branch, every
# tag), not just from HEAD. Removing a file with `git rm` only deletes it
# going forward; the old blobs stay reachable in history and on every fork
# /clone. This script is the only way to actually un-leak files that were
# already pushed to a public remote.
#
# DEFAULT BEHAVIOUR IS A DRY RUN. Nothing is rewritten unless you pass
# --confirm. Even with --confirm, the final `git push --force` lines are
# left COMMENTED OUT on purpose — a human runs those deliberately after
# reviewing the rewritten history locally.
#
# Tooling: prefers `git filter-repo` (the modern, supported tool). A
# `git filter-branch` fallback is documented below for environments that
# cannot install filter-repo, but filter-branch is slow, error-prone, and
# officially discouraged — install filter-repo if you possibly can:
#   pipx install git-filter-repo      # or: brew install git-filter-repo
#                                      # or: pip install git-filter-repo
#
set -euo pipefail

# ---------------------------------------------------------------------------
# Paths to purge from ALL history. Keep this list in sync with the working-
# tree removals. These are repo-root-relative paths (forward slashes).
# ---------------------------------------------------------------------------
PATHS=(
  "studio/render_eden.py"
  "studio/render_eden_wan.py"
  "studio/render_hunyuan.py"
  "studio/load_ltx2.py"
  "studio/render_scenes.py"
  "studio/patent_lab.sw"
  "studio/research_lab.sw"
  "studio/research_agent_v2.sw"
  "studio/video_studio.sw"
  "docs/notes/AUDIT_REPORT.md"
  "docs/notes/VALIDATION_REPORT.md"
)

CONFIRM=0
for arg in "$@"; do
  case "$arg" in
    --confirm) CONFIRM=1 ;;
    -h|--help)
      grep '^#' "$0" | sed 's/^# \{0,1\}//'
      exit 0
      ;;
    *)
      echo "Unknown argument: $arg (use --confirm to actually rewrite, or --help)" >&2
      exit 2
      ;;
  esac
done

# ---------------------------------------------------------------------------
# Loud warning banner.
# ---------------------------------------------------------------------------
cat <<'BANNER'
###############################################################################
#                                                                             #
#   !!!  DESTRUCTIVE GIT HISTORY REWRITE  !!!                                  #
#                                                                             #
#   This rewrites EVERY commit SHA in the repository. Anyone who has          #
#   cloned or forked this repo will have a diverged history and must          #
#   re-clone. There is NO undo once you force-push to the public remote.      #
#                                                                             #
#   MANDATORY: take a full mirror backup FIRST, before running with          #
#   --confirm:                                                                #
#                                                                             #
#       git clone --mirror . ../swarmrt-backup-$(date +%Y%m%d).git            #
#                                                                             #
#   (a --mirror clone captures all refs: branches, tags, notes.)             #
#                                                                             #
###############################################################################
BANNER
echo

echo "Paths that will be removed from ALL history:"
for p in "${PATHS[@]}"; do
  echo "    - $p"
done
echo

# ---------------------------------------------------------------------------
# Dry run (default): print the exact command and exit without touching git.
# ---------------------------------------------------------------------------
if [[ "$CONFIRM" -ne 1 ]]; then
  echo "[DRY RUN] No changes made. Re-run with --confirm to rewrite history."
  echo
  echo "Would run (git filter-repo):"
  printf '    git filter-repo --force'
  for p in "${PATHS[@]}"; do
    printf ' \\\n        --path %q --invert-paths' "$p"
  done
  printf '\n'
  echo
  echo "Reminder: back up first with:"
  echo "    git clone --mirror . ../swarmrt-backup-\$(date +%Y%m%d).git"
  exit 0
fi

# ---------------------------------------------------------------------------
# --confirm path: actually rewrite history (still does NOT push).
# ---------------------------------------------------------------------------
if ! command -v git >/dev/null 2>&1; then
  echo "ERROR: git not found on PATH." >&2
  exit 1
fi

# Sanity: must be at the top of a git work tree.
if ! git rev-parse --is-inside-work-tree >/dev/null 2>&1; then
  echo "ERROR: not inside a git repository." >&2
  exit 1
fi

echo ">>> Backup check: have you already run 'git clone --mirror' (see banner)?"
echo ">>> Sleeping 5s so you can Ctrl-C if not..."
sleep 5

if command -v git-filter-repo >/dev/null 2>&1 || git filter-repo --help >/dev/null 2>&1; then
  echo ">>> Using git filter-repo to remove paths from all history..."
  FILTER_ARGS=( --force )
  for p in "${PATHS[@]}"; do
    FILTER_ARGS+=( --path "$p" --invert-paths )
  done
  git filter-repo "${FILTER_ARGS[@]}"
  echo ">>> filter-repo complete."
else
  echo "ERROR: git-filter-repo is not installed." >&2
  echo "Install it (recommended):" >&2
  echo "    pipx install git-filter-repo   # or brew install git-filter-repo" >&2
  echo >&2
  echo "----------------------------------------------------------------------" >&2
  echo "FALLBACK (discouraged): git filter-branch. Slow and error-prone." >&2
  echo "Run MANUALLY only if you cannot install filter-repo. Example:" >&2
  echo >&2
  echo "    git filter-branch --force --index-filter '" >&2
  echo "        git rm -r --cached --ignore-unmatch \\" >&2
  for p in "${PATHS[@]}"; do
    echo "            \"$p\" \\" >&2
  done
  echo "    ' --prune-empty --tag-name-filter cat -- --all" >&2
  echo >&2
  echo "    # then expire reflogs + gc to drop the old blobs:" >&2
  echo "    git for-each-ref --format='delete %(refname)' refs/original | git update-ref --stdin" >&2
  echo "    git reflog expire --expire=now --all" >&2
  echo "    git gc --prune=now --aggressive" >&2
  echo "----------------------------------------------------------------------" >&2
  exit 1
fi

echo
echo ">>> History rewrite done LOCALLY. Verify before pushing:"
echo "        git log --all --oneline | head"
echo "        git log --all --full-history -- studio/patent_lab.sw   # should be EMPTY"
echo
echo ">>> The force-push is intentionally NOT run by this script."
echo ">>> Review the rewritten history, then run these by hand, deliberately:"
echo
# -------------------------------------------------------------------------
# DO NOT UNCOMMENT CASUALLY. Force-pushing rewrites PUBLIC history that other
# people may already have cloned/forked. Every collaborator must re-clone.
# The human operator runs these manually after reviewing the local rewrite.
# -------------------------------------------------------------------------
#   git push origin --force --all
#   git push origin --force --tags
echo "    # git push origin --force --all     (commented out — run deliberately)"
echo "    # git push origin --force --tags    (commented out — run deliberately)"
echo
echo ">>> After pushing: contact anyone with a clone/fork to re-clone, and"
echo ">>> ask GitHub Support to purge cached views of the removed blobs."

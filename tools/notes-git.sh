#!/usr/bin/env bash
# notes-git.sh — thin wrapper around the project's second, local-only git
# repo (.notes.git) that tracks xnote/, WORKLOG.md, AGENTS.md, plan.md, and
# FEATURES.md separately from the main repo. Those paths are gitignored here
# on purpose and never pushed to this project's public GitHub remote.
#
# Usage: tools/notes-git.sh <any git subcommand>
#   tools/notes-git.sh status
#   tools/notes-git.sh add -A
#   tools/notes-git.sh commit -m "docs: session N worklog"
#   tools/notes-git.sh log --oneline

set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
exec git --git-dir="$ROOT/.notes.git" --work-tree="$ROOT" "$@"

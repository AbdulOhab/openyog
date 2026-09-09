#!/usr/bin/env bash
# ui-compare.sh — capture OpenYog and the real SQLyog at the same size and
# stitch them side by side, so the owner reviews ONE image per session instead
# of describing the visual gap in words.
#
# See xnote/2026-09-09-ui-shell-spec.md for why this exists (owner directive:
# converge on the look first, judged per side-by-side screenshot).
#
# Usage:
#   tools/ui-compare.sh [tag]
#
#   tag   basename for the output files (default: date+time)
#
# Env overrides:
#   OY_BIN        path to the openyog binary   (default: build-cmake/openyog)
#   SQLYOG_EXE    path to SQLyogCommunity.exe  (default: autodetect under ~/.wine)
#   OY_ARGS       extra args for openyog, e.g. --autoconnect=127.0.0.1:3307:port:port123:port_test
#                 (without a connection both apps just show the empty shell —
#                  pass --autoconnect to compare a populated layout)
#   SIZE          WxH for both windows         (default: 1200x760, matches MainWindow::resize)
#   OUT_DIR       where to write pngs          (default: ../ next to the repo)
#   SQLYOG_WIN_RE window-title regex for xdotool (default: 'SQLyog Community')
#
# Requires: wine, xdotool, ImageMagick (magick/import). SQLyog must already be
# installed in the active WINEPREFIX.

set -euo pipefail
cd "$(dirname "$0")/.."

tag="${1:-$(date +%Y%m%d-%H%M%S)}"
OY_BIN="${OY_BIN:-build-cmake/openyog}"
SIZE="${SIZE:-1200x760}"
OUT_DIR="${OUT_DIR:-..}"
W="${SIZE%x*}"; H="${SIZE#*x}"

ours="${OUT_DIR}/cmp-${tag}-openyog.png"
theirs="${OUT_DIR}/cmp-${tag}-sqlyog.png"
combo="${OUT_DIR}/cmp-${tag}.png"

# ---------------------------------------------------------------- OpenYog side
echo "[ui-compare] capturing OpenYog -> $ours"
QT_QPA_PLATFORM=offscreen "$OY_BIN" --screenshot="$ours" ${OY_ARGS:-}

# ---------------------------------------------------------------- SQLyog side
if [[ -z "${SQLYOG_EXE:-}" ]]; then
    SQLYOG_EXE="$(find "${WINEPREFIX:-$HOME/.wine}" -iname 'SQLyogCommunity.exe' 2>/dev/null | head -1 || true)"
fi
if [[ -z "$SQLYOG_EXE" || ! -f "$SQLYOG_EXE" ]]; then
    echo "[ui-compare] SQLyog not found (set SQLYOG_EXE=...); OpenYog shot only: $ours"
    exit 0
fi

echo "[ui-compare] launching SQLyog via wine: $SQLYOG_EXE"
wine "$SQLYOG_EXE" >/dev/null 2>&1 &
wine_pid=$!
cleanup() { kill "$wine_pid" 2>/dev/null || true; wineserver -k 2>/dev/null || true; }
trap cleanup EXIT

# wait for the top-level SQLyog window. Match the real title ("SQLyog Community
# - [ ... ]"), never the repo folder name or the editor; reject known false hits.
win_re="${SQLYOG_WIN_RE:-SQLyog Community}"
win=""
for _ in $(seq 1 40); do
    for cand in $(xdotool search --name "$win_re" 2>/dev/null || true); do
        name="$(xdotool getwindowname "$cand" 2>/dev/null || true)"
        case "$name" in
            *"Visual Studio Code"*|*"Code - "*|*"- code"*) continue ;;
        esac
        [[ -n "$name" ]] && { win="$cand"; break; }
    done
    [[ -n "$win" ]] && break
    sleep 0.5
done
if [[ -z "$win" ]]; then
    echo "[ui-compare] no window matching /$win_re/ (set SQLYOG_WIN_RE=...);" \
         "OpenYog shot only: $ours"
    echo "[ui-compare] open windows:"; wmctrl -l 2>/dev/null | sed 's/^/    /' || true
    exit 0
fi
echo "[ui-compare] matched SQLyog window: $(xdotool getwindowname "$win")"

xdotool windowsize "$win" "$W" "$H"
xdotool windowactivate --sync "$win"
sleep 1.5
import -window "$win" "$theirs"
echo "[ui-compare] captured SQLyog -> $theirs"

# ---------------------------------------------------------------- stitch
magick "$ours" "$theirs" -background '#888' -gravity north +append "$combo"
echo "[ui-compare] side-by-side -> $combo   (left: OpenYog, right: SQLyog)"

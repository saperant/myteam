#!/bin/sh
# The README's screenshots (docs/screenshots), from a made-up org in a sandbox:
#   tools/screenshots/make.sh [MYRCP_BUILD_DIR]
# Needs build/myteam and build/myorg, and .venv with ui/ (pip install -e ui) and pyte; the
# GTK ones a myrcp build with myrcp-gtk (cmake -DMYRCP_GTK=ON) given as the argument, Xvfb,
# xdotool and ImageMagick. No agent runs: the crew's work is written by demo_setup.sh.
set -e
HERE=$(cd "$(dirname "$0")" && pwd); REPO=$(dirname "$(dirname "$HERE")")
OUT=$REPO/docs/screenshots; WORK=$(mktemp -d); trap 'rm -rf "$WORK"' EXIT
mkdir -p "$OUT" "$WORK/demo/run"; chmod 700 "$WORK/demo/run"
"$HERE/sandbox.sh" "$WORK/demo" sh "$HERE/demo_setup.sh"
"$HERE/sandbox.sh" "$WORK/demo" python "$HERE/rcp_shots.py" "$WORK"
cp "$WORK"/rcp-*.svg "$OUT/"
if [ -n "$1" ]; then
  MYRCP_GTK_DIR=$1 "$HERE/sandbox.sh" "$WORK/demo" python "$HERE/gtk_shots.py" "$WORK" \
    "key:ctrl+p" "sleep:0.5" "type:perspective myteam" "sleep:0.6" "key:Return" "sleep:3" "shot:gtk-overview" \
    "key:alt+2" "sleep:1.5" "shot:gtk-plan" \
    "key:alt+5" "sleep:1.5" "shot:gtk-coach" \
    "key:ctrl+p" "sleep:0.5" "type:perspective myorg" "sleep:0.6" "key:Return" "sleep:4" "shot:gtk-topology" \
    "click:63,69" "sleep:2" "shot:gtk-org"
  for f in gtk-overview gtk-plan gtk-coach gtk-topology gtk-org; do
    convert "$WORK/$f.png" -strip -colors 256 "PNG8:$OUT/$f.png"
  done
fi
ls -l "$OUT"

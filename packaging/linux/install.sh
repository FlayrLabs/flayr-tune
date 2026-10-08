#!/bin/sh
# Installs Flayr Tune for the current user: VST3 into ~/.vst3 and LV2 into ~/.lv2.
# Run with --uninstall to remove it again.
set -e

here="$(cd "$(dirname "$0")" && pwd)"
vst3="$HOME/.vst3/Flayr Tune.vst3"
lv2="$HOME/.lv2/Flayr Tune.lv2"

if [ "${1:-}" = "--uninstall" ]; then
    rm -rf "$vst3" "$lv2"
    echo "Flayr Tune removed."
    exit 0
fi

mkdir -p "$HOME/.vst3" "$HOME/.lv2"
rm -rf "$vst3" "$lv2"
cp -R "$here/Flayr Tune.vst3" "$HOME/.vst3/"
cp -R "$here/Flayr Tune.lv2" "$HOME/.lv2/"

echo "Flayr Tune installed:"
echo "  VST3: $vst3"
echo "  LV2:  $lv2"
echo "Restart your DAW or rescan plug-ins (Reaper, Bitwig, Ardour, Carla...)."

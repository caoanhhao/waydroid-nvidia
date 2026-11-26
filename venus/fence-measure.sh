#!/bin/sh
# Measure frame pacing inside Waydroid for the sync-fd fence comparison.
# Run as your user with Waydroid running and unlocked to the home screen;
# it calls `sudo waydroid shell` (one sudo prompt).
#
#   sh venus/fence-measure.sh before    # with the release (patch 0001) stack
#   sh venus/fence-measure.sh after     # after venus/deploy-fence.sh install
#
# It scrolls Settings and pulls the notification shade for a fixed number of
# rounds, then saves gfxinfo (Settings + SystemUI), SurfaceFlinger timestats and
# the count of "failed to dup EGL native fence sync" warnings to
# venus/fence-measure-<label>.txt, and prints a summary.
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
LABEL=${1:?usage: $0 before|after [rounds]}
ROUNDS=${2:-15}
OUT=$ROOT/venus/fence-measure-$LABEL.txt

ANDROID_SCRIPT='
R='"$ROUNDS"'
set -- $(wm size | sed -n "s/.*: \([0-9]*\)x\([0-9]*\).*/\1 \2/p" | tail -1)
W=${1:-1080}; H=${2:-1920}
X=$((W / 2)); Y1=$((H * 4 / 5)); Y2=$((H / 5))

logcat -c
dumpsys SurfaceFlinger --timestats -disable >/dev/null
dumpsys SurfaceFlinger --timestats -clear -enable >/dev/null

am start -W -a android.settings.SETTINGS >/dev/null
# give Settings time to replace its splash screen with real content
sleep 6
dumpsys gfxinfo com.android.settings reset >/dev/null
dumpsys gfxinfo com.android.systemui reset >/dev/null

i=0
while [ $i -lt $R ]; do
    input swipe $X $Y1 $X $Y2 120
    input swipe $X $Y2 $X $Y1 120
    i=$((i + 1))
done
i=0
while [ $i -lt $R ]; do
    cmd statusbar expand-notifications; sleep 0.6
    cmd statusbar collapse; sleep 0.6
    i=$((i + 1))
done

echo "=== screen ${W}x${H}, rounds $R"
echo "=== top activity: $(dumpsys activity activities | grep -m1 mResumedActivity)"
echo "=== gfxinfo com.android.settings"
dumpsys gfxinfo com.android.settings | sed -n "/^Stats since/,/^Number Frame deadline missed/p;/Pipeline=/p"
echo "=== gfxinfo com.android.systemui"
dumpsys gfxinfo com.android.systemui | sed -n "/^Stats since/,/^Number Frame deadline missed/p;/Pipeline=/p"
echo "=== SurfaceFlinger timestats"
dumpsys SurfaceFlinger --timestats -dump -maxlayers 3
dumpsys SurfaceFlinger --timestats -disable >/dev/null
echo "=== logcat"
echo "failed_dup_native_fence $(logcat -d | grep -c "failed to dup EGL native fence sync")"
echo "vn_errors $(logcat -d | grep -ci "venus\|vn_.*fail\|VK_ERROR" )"
'

echo "measuring ($LABEL, $ROUNDS rounds); this takes about a minute"
sudo waydroid shell -- sh -c "$ANDROID_SCRIPT" >"$OUT" 2>&1
echo "saved $OUT"
echo
for app in settings systemui; do
    echo "--- $app"
    sed -n "/=== gfxinfo com.android.$app/,/^===/p" "$OUT" |
        grep -E "Total frames|Janky frames|percentile|Missed Vsync|Slow issue|Pipeline" |
        grep -v "^===" | head -14
done
echo "--- SurfaceFlinger"
grep -E "^(totalFrames|missedFrames|clientCompositionFrames|averageFPS|displayOnTime) " "$OUT" | head -6
grep -A1 "present2present histogram" "$OUT" | head -2
echo "--- warnings"
grep -E "^(failed_dup_native_fence|vn_errors) " "$OUT"

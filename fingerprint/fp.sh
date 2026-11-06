#!/bin/sh
# Drive Waydroid's virtual fingerprint sensor over adb (install it with fingerprint/install.sh).
#
#   sh fingerprint/fp.sh enroll [id]      enroll finger <id> (default: next free) through Settings
#   sh fingerprint/fp.sh touch [id]       put enrolled finger <id> (default: first) on the sensor
#   sh fingerprint/fp.sh bad              put an unknown finger on the sensor
#   sh fingerprint/fp.sh lockout [on|off] lock fingerprint out (on: also touches once) / clear it
#   sh fingerprint/fp.sh place [x:y:r]    move the sensor (default: lower middle of the display)
#   sh fingerprint/fp.sh status
#
# The sensor is an under-display one: a finger is a 0.8 s press on the sensor spot, which
# SystemUI hands to the HAL; the HAL then matches vendor.fingerprint.virtual.enrollment_hit
# against persist.vendor.fingerprint.virtual.enrollments.
# adb: uses $ANDROID_SERIAL if set, else connects to the container IP from `waydroid status`.
# FP_PIN=1234 sh fingerprint/fp.sh enroll ...   types the lock screen PIN when Settings asks.
set -eu
P=vendor.fingerprint.virtual
PP=persist.vendor.fingerprint.virtual

if [ -z "${ANDROID_SERIAL:-}" ]; then
    ip=$(waydroid status 2>/dev/null | sed -n 's/^IP address:[[:space:]]*//p')
    [ -n "$ip" ] || { echo "Waydroid is not running (no IP in waydroid status)" >&2; exit 1; }
    ANDROID_SERIAL=$ip:5555
    adb connect "$ANDROID_SERIAL" >/dev/null
fi
export ANDROID_SERIAL
sh_() { adb shell "$@" | tr -d '\r'; }

enrolled() { sh_ getprop $PP.enrollments | tr ',' ' '; }

# Where to press: the sensor centre from the HAL props, moved by the offset WM applies to
# SystemUI's overlay window when it does not fit on the display (Settings' enroll overlay
# is larger than a narrow Waydroid window, so WM clamps it and the hit area moves with it).
press() {
    loc=$(sh_ getprop $PP.sensor_location)
    [ -n "$loc" ] || loc=400:1600:150 # the HAL's built-in default
    sx=${loc%%:*}; rest=${loc#*:}; sy=${rest%%:*}
    sh_ input keyevent KEYCODE_WAKEUP
    sleep 0.3
    win=$(sh_ dumpsys window windows | awk '
        /Window #/ { inwin = /UdfpsControllerOverlay/ }
        inwin && /mAttrs=/ && !a { a = $0 }
        inwin && / frame=/ && !f { f = $0 }
        END { print a; print f }')
    req=$(printf '%s\n' "$win" | sed -n '1s/.*mAttrs={(\(-\{0,1\}[0-9]*\),\(-\{0,1\}[0-9]*\)).*/\1 \2/p')
    frm=$(printf '%s\n' "$win" | sed -n '2s/.* frame=\[\(-\{0,1\}[0-9]*\),\(-\{0,1\}[0-9]*\)\].*/\1 \2/p')
    if [ -z "$req" ] || [ -z "$frm" ]; then
        [ "${1:-}" = quiet ] && return 0 # enroll: the overlay is not up yet, try again later
        echo "warning: no UDFPS overlay on screen, nothing is waiting for a finger" >&2
    else
        set -- $req $frm
        sx=$((sx - $1 + $3)); sy=$((sy - $2 + $4))
    fi
    sh_ input swipe "$sx" "$sy" "$((sx + 1))" "$((sy + 1))" 800
}

# primary (bottom-right) button of the current Settings screen
tap_primary() {
    sh_ uiautomator dump /sdcard/fp-ui.xml >/dev/null 2>&1 || return 0
    xy=$(sh_ cat /sdcard/fp-ui.xml | tr '>' '\n' | grep 'clickable="true"' |
        sed -n 's/.*bounds="\[\([0-9]*\),\([0-9]*\)\]\[\([0-9]*\),\([0-9]*\)\]".*/\1 \2 \3 \4/p' |
        awk '{ x = int(($1 + $3) / 2); y = int(($2 + $4) / 2)
               if (y > by || (y == by && x > bx)) { bx = x; by = y } }
             END { if (by) print bx, by }')
    [ -z "$xy" ] || sh_ input tap $xy
}

top_activity() {
    sh_ dumpsys activity activities | sed -n 's/.*topResumedActivity=ActivityRecord{[^ ]* [^ ]* \([^ }]*\).*/\1/p' | head -1
}

cmd_enroll() {
    id=${1:-}
    if [ -z "$id" ]; then
        id=1
        for e in $(enrolled); do [ "$e" -lt "$id" ] || id=$((e + 1)); done
    fi
    # 5 captures of 300 ms each, then success; the HAL clears this once enrolled
    sh_ setprop $P.next_enrollment "$id:300,300,300,300,300:true"
    sh_ input keyevent KEYCODE_WAKEUP
    sh_ am start -a android.settings.FINGERPRINT_ENROLL >/dev/null
    echo "enrolling finger $id"
    asked=
    i=0
    while [ $i -lt 40 ]; do
        i=$((i + 1))
        sleep 1.5
        case $(top_activity) in
        *ConfirmLock*)
            [ -z "$asked" ] || continue
            asked=1
            if [ -n "${FP_PIN:-}" ]; then
                sh_ input text "$FP_PIN"
                sh_ input keyevent KEYCODE_ENTER
            else
                echo "enter your lock screen PIN/password in the Waydroid window (or set FP_PIN)"
            fi ;;
        *FingerprintEnrollIntroduction | *FingerprintEnrollFindSensor)
            tap_primary ;; # More, I agree, Start
        *FingerprintEnrollEnrolling)
            press quiet ;;
        *FingerprintEnrollFinish)
            tap_primary # Done
            break ;;
        *ChooseLock*)
            echo "Android has no screen lock: set a PIN first (Settings > Security, or" \
                "adb shell locksettings set-pin <PIN>)" >&2
            exit 1 ;;
        esac
    done
    case " $(enrolled) " in
    *" $id "*) echo "enrolled: $(enrolled)" ;;
    *) echo "finger $id was not enrolled (enrollments: $(enrolled))" >&2; exit 1 ;;
    esac
}

cmd_touch() {
    id=${1:-$(enrolled | awk '{ print $1 }')}
    [ -n "$id" ] || { echo "no finger is enrolled; run: sh $0 enroll" >&2; exit 1; }
    sh_ setprop $P.enrollment_hit "$id"
    press
}

cmd_bad() {
    sh_ setprop $P.enrollment_hit 0
    press
}

cmd_lockout() {
    case ${1:-on} in
    on)
        sh_ setprop $PP.lockout true
        press # the HAL reports lockout to whoever is listening
        echo "locked out; unlocking with the PIN clears it" ;;
    off) sh_ setprop $PP.lockout false ;;
    *) echo "usage: $0 lockout [on|off]" >&2; exit 1 ;;
    esac
}

cmd_place() {
    loc=${1:-}
    if [ -z "$loc" ]; then
        size=$(sh_ wm size | sed -n 's/.*size: \([0-9]*\)x\([0-9]*\).*/\1 \2/p' | tail -1)
        set -- $size
        r=$(($1 / 10)); [ $r -ge 24 ] || r=24
        loc=$(($1 / 2)):$(($2 * 85 / 100)):$r
    fi
    sh_ setprop $PP.sensor_location "$loc"
    echo "sensor at $loc; restart Waydroid (waydroid session stop) for it to take effect"
}

cmd_status() {
    echo "device:      $ANDROID_SERIAL ($(sh_ wm size | sed -n 's/.*size: //p' | tail -1))"
    echo "type:        $(sh_ getprop $PP.type)  sensor at $(sh_ getprop $PP.sensor_location)"
    echo "enrollments: $(enrolled)"
    echo "finger hit:  $(sh_ getprop $P.enrollment_hit)"
    echo "lockout:     $(sh_ getprop $PP.lockout)"
    echo "keyguard:    $(sh_ dumpsys window policy | sed -n 's/^ *showing=//p' | head -1 |
        sed 's/true/showing/; s/false/hidden/')"
    sh_ dumpsys fingerprint | sed -n 's/^Dumping for sensorId: /sensor id:   /p; /"prints"/p; s/^Current operation: /busy:        /p'
}

case "${1:-}" in
enroll | touch | bad | lockout | place | status) c=$1; shift; "cmd_$c" "$@" ;;
*) sed -n '2,17p' "$0" | sed 's/^# \{0,1\}//'; exit 1 ;;
esac

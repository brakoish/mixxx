#!/bin/bash
# Deck Lab gpu-screen spike: run the QML Decks preview directly on the display through Qt's
# EGLFS/KMS backend (no X server, no compositor, no native child windows) and log CPU per thread
# and frames per second. EXPERIMENTAL. Never part of the product image.
#
#   run-qml-eglfs.sh check             read-only: display, touch, flatpak ref and kiosk state
#   run-qml-eglfs.sh stop-kiosk        sudo: stop Mixxx and the tty1 Xorg kiosk (nothing restarts it)
#   run-qml-eglfs.sh run [SECONDS]     run the QML preview for SECONDS (default 120), then quit it
#   run-qml-eglfs.sh restore-kiosk     sudo: exact revert, the kiosk starts again on tty1
#
# Run as the DJ user from a text console (Ctrl+Alt+F2) or over SSH. Prerequisite: the QML candidate
# from CI installed beside the product, which it never replaces:
#   flatpak install --user ./decklab-mixxx-qml-aarch64-<rev>.flatpak     # ref org.mixxx.Mixxx//decklab-qml
#
# Optional environment:
#   DECKLAB_QML_REF       flatpak ref (default org.mixxx.Mixxx//decklab-qml)
#   DECKLAB_PROFILE       Mixxx profile to copy (default ~/.var/app/org.mixxx.Mixxx/.mixxx). The run
#                         uses a COPY in ~/.cache/decklab-qml/profile; the real profile is never written.
#   DECKLAB_DRM_CARD      e.g. /dev/dri/card1 (default: the card with a connected DSI, else HDMI)
#   DECKLAB_DRM_OUTPUT    e.g. DSI-1 or HDMI-A-1 (default: connected DSI first, then HDMI)
#   DECKLAB_TOUCH_DEVICE  e.g. /dev/input/event4 (default: first device matching TOUCH_MATCH)
#   TOUCH_MATCH           extended regex for the touch device name (default as the kiosk)
#   DECKLAB_RENDER_GATE   0 = upstream every-frame waveform rendering (A/B against the default gate)
#   QSG_RENDER_LOOP       basic or threaded (default: Qt's choice, logged by QSG_INFO)
#   DECKLAB_KMS_ATOMIC    1 = atomic KMS commits
#   SAMPLE_SECONDS        CPU sample interval (default 5)
set -u

ref=${DECKLAB_QML_REF:-org.mixxx.Mixxx//decklab-qml}
profile_src=${DECKLAB_PROFILE:-}
if [ -z "$profile_src" ] && [ -r "$HOME/.config/decklab/mixxx.env" ]; then
    # The kiosk's per-user override (decklab-mixxx.service EnvironmentFile).
    profile_src=$(sed -n 's/^DECKLAB_PROFILE=//p' "$HOME/.config/decklab/mixxx.env" | tail -n 1 | tr -d "\"'")
    profile_src=${profile_src/#\~/$HOME}
fi
profile_src=${profile_src:-$HOME/.var/app/org.mixxx.Mixxx/.mixxx}
work=$HOME/.cache/decklab-qml
touch_match=${TOUCH_MATCH:-'[Tt]ouch|ft5x06|[Gg]oodix|ILITEK|eGalax|[Dd]igitizer|[Cc]apacitive'}
sample=${SAMPLE_SECONDS:-5}

die() { echo "run-qml-eglfs: $*" >&2; exit 1; }
note() { echo "run-qml-eglfs: $*"; }

# DRM output: a connected DSI panel first, then HDMI (same order as the kiosk's display=auto).
find_output() {
    local want=${DECKLAB_DRM_OUTPUT:-} status c name
    for kind in DSI HDMI-A; do
        for c in /sys/class/drm/card*-"$kind"-*; do
            [ -e "$c" ] || continue
            name=${c##*/}
            status=$(cat "$c/status" 2>/dev/null)
            if [ -n "$want" ]; then
                [ "${name#*-}" = "$want" ] || continue
            else
                [ "$status" = connected ] || continue
            fi
            echo "$name"
            return 0
        done
        [ -n "$want" ] && continue
    done
    return 1
}

# Qt's eglfs_kms names connectors without the dash: DSI-1 -> DSI1, HDMI-A-1 -> HDMI1.
qt_output_name() { echo "$1" | sed -e 's/^HDMI-A-/HDMI/' -e 's/^\([A-Z]*\)-\([0-9]\)$/\1\2/'; }

find_touch() {
    if [ -n "${DECKLAB_TOUCH_DEVICE:-}" ]; then
        echo "$DECKLAB_TOUCH_DEVICE"
        return 0
    fi
    awk -v re="$touch_match" '
        /^N: Name=/ { name = $0; sub(/^N: Name="/, "", name); sub(/"$/, "", name) }
        /^H: Handlers=/ { if (name ~ re && match($0, /event[0-9]+/)) { print "/dev/input/" substr($0, RSTART, RLENGTH); exit } }
    ' /proc/bus/input/devices
}

kiosk_running() { pgrep -x Xorg >/dev/null 2>&1 || pgrep -x labwc >/dev/null 2>&1; }

cmd_check() {
    local output card mode touch
    output=$(find_output) || die "no connected DSI or HDMI output under /sys/class/drm"
    card=${DECKLAB_DRM_CARD:-/dev/dri/${output%%-*}}
    mode=$(head -n 1 "/sys/class/drm/$output/modes" 2>/dev/null)
    touch=$(find_touch)
    echo "output      $output (Qt name $(qt_output_name "${output#*-}")) mode ${mode:-unknown} on $card"
    echo "touch       ${touch:-none found (set DECKLAB_TOUCH_DEVICE)}"
    echo "flatpak     $(flatpak info --show-commit "$ref" 2>/dev/null || echo "$ref NOT installed")"
    echo "profile     $profile_src $([ -d "$profile_src" ] && echo present || echo MISSING)"
    if kiosk_running; then
        echo "kiosk       RUNNING (Xorg/labwc owns the display): run '$0 stop-kiosk' first"
    else
        echo "kiosk       stopped"
    fi
    echo "groups      $(id -nG)"
}

cmd_stop_kiosk() {
    note "stopping the Deck Lab kiosk (sudo asks for your password)"
    # Same mechanism as decklab-desktop: the flag keeps decklab-session from restarting the
    # display; it lives in /run, so a reboot also brings the kiosk back.
    sudo install -d -m 0755 /run/decklab || die "sudo failed"
    sudo touch /run/decklab/desktop-mode
    systemctl --user stop decklab-mixxx.service 2>/dev/null || note "decklab-mixxx.service was not running"
    sudo systemctl stop getty@tty1.service
    for _ in 1 2 3 4 5 6 7 8 9 10; do
        kiosk_running || break
        sleep 1
    done
    if kiosk_running; then
        note "Xorg still running; terminating it"
        sudo pkill -TERM -x Xorg
        sleep 2
    fi
    kiosk_running && die "the display server is still running"
    note "kiosk stopped. Revert with: $0 restore-kiosk   (or reboot)"
}

cmd_restore_kiosk() {
    note "restoring the Deck Lab kiosk on tty1"
    pkill -TERM -x mixxx.bin 2>/dev/null && sleep 3
    sudo rm -f /run/decklab/desktop-mode
    sudo systemctl restart getty@tty1.service
    note "tty1 autologin restarts decklab-session -> Xorg -> decklab-mixxx.service"
}

# Per-thread CPU from /proc/<pid>/task/*/stat (utime+stime ticks), one line per interval.
sample_threads() { # sample_threads PID SECONDS -> "tid comm ticks" lines
    local pid=$1 t
    for t in /proc/"$pid"/task/*; do
        [ -r "$t/stat" ] || continue
        # comm may contain spaces: fields after the last ')' start with state (field 3).
        awk -v tid="${t##*/}" -v comm="$(cat "$t/comm" 2>/dev/null | tr ' ' '_')" '{
            line = $0; sub(/.*\) /, "", line); split(line, f, " ");
            print tid, comm, f[12] + f[13] }' "$t/stat"
    done
}

irq_count() { awk -v re="$1" '$0 ~ re { for (i = 2; i <= NF && $i ~ /^[0-9]+$/; i++) n += $i } END { print n + 0 }' /proc/interrupts; }

cmd_run() {
    local seconds=${1:-120} output card mode touch qtname arch logdir pid hz
    [ -z "${DISPLAY:-}${WAYLAND_DISPLAY:-}" ] || die "run from a text console or SSH, not inside a desktop session"
    kiosk_running && die "Xorg/labwc owns the display. Run '$0 stop-kiosk' first"
    flatpak info "$ref" >/dev/null 2>&1 || die "$ref is not installed (flatpak install --user ./decklab-mixxx-qml-aarch64-*.flatpak)"
    [ -d "$profile_src" ] || die "profile $profile_src does not exist (set DECKLAB_PROFILE)"
    output=$(find_output) || die "no connected DSI or HDMI output"
    card=${DECKLAB_DRM_CARD:-/dev/dri/${output%%-*}}
    [ -r "$card" ] && [ -w "$card" ] || die "no access to $card (the DJ user must be in the video group)"
    mode=$(head -n 1 "/sys/class/drm/$output/modes" 2>/dev/null)
    qtname=$(qt_output_name "${output#*-}")
    touch=$(find_touch)
    arch=$(uname -m)-linux-gnu
    hz=$(getconf CLK_TCK)

    logdir=$work/run-$(date +%Y%m%d-%H%M%S)
    mkdir -p "$logdir" "$work/profile"
    # Fresh copy of the DJ's settings and library database; the analysis cache is shared read-only.
    find "$work/profile" -mindepth 1 -maxdepth 1 -exec rm -rf {} +
    (cd "$profile_src" && find . -mindepth 1 -maxdepth 1 ! -name analysis -exec cp -a {} "$work/profile/" \;)
    [ -d "$profile_src/analysis" ] && ln -s "$profile_src/analysis" "$work/profile/analysis"

    # One output, the panel's preferred mode; every other connector off.
    {
        printf '{\n  "device": "%s",\n  "hwcursor": false,\n  "pbuffers": true,\n  "outputs": [\n' "$card"
        printf '    { "name": "%s", "mode": "%s" }' "$qtname" "${mode:-current}"
        for c in /sys/class/drm/"${output%%-*}"-*; do
            n=${c##*/}; n=${n#*-}
            [ "$n" = "${output#*-}" ] && continue
            case $n in Writeback-*) continue ;; esac
            printf ',\n    { "name": "%s", "mode": "off" }' "$(qt_output_name "$n")"
        done
        printf '\n  ]\n}\n'
    } >"$logdir/kms.json"

    args=(run --die-with-parent
        --nosocket=x11 --nosocket=fallback-x11 --nosocket=wayland
        "--filesystem=$work"
        "--filesystem=$logdir"
        --filesystem=/run/udev:ro
        --env=QT_QPA_PLATFORM=eglfs
        --env=QT_QPA_EGLFS_INTEGRATION=eglfs_kms
        "--env=QT_QPA_EGLFS_KMS_CONFIG=$logdir/kms.json"
        --env=QT_QPA_EGLFS_ALWAYS_SET_MODE=1
        --env=QT_QPA_EGLFS_HIDECURSOR=1
        --env=QT_QPA_EGLFS_NO_LIBINPUT=1
        "--env=GBM_BACKENDS_PATH=/usr/lib/$arch/GL/default/lib/gbm"
        --env=QSG_INFO=1
        --env=DECKLAB_QML_MAIN=qml/DeckLab/main.qml
        --env=DECKLAB_PERF_LOG=1
        --env=DECKLAB_AUDIO_CPU=last
        --env=DECKLAB_ANALYZER_IDLE=1)
    [ -d "$profile_src/analysis" ] && args+=("--filesystem=$profile_src/analysis:ro")
    [ -n "$touch" ] && args+=("--env=QT_QPA_EVDEV_TOUCHSCREEN_PARAMETERS=$touch")
    [ -n "${DECKLAB_RENDER_GATE:-}" ] && args+=("--env=DECKLAB_RENDER_GATE=$DECKLAB_RENDER_GATE")
    [ -n "${QSG_RENDER_LOOP:-}" ] && args+=("--env=QSG_RENDER_LOOP=$QSG_RENDER_LOOP")
    [ "${DECKLAB_KMS_ATOMIC:-}" = 1 ] && args+=(--env=QT_QPA_EGLFS_KMS_ATOMIC=1)
    # Mixxx prints only warnings to the console by default; the fps and GL lines are info.
    args+=("$ref" --qml "--settings-path=$work/profile" --resource-path=/app/share/mixxx --log-level info)

    note "output $output ($qtname, ${mode:-current}) on $card; touch ${touch:-none}"
    note "logs in $logdir"
    echo "flatpak ${args[*]}" >"$logdir/command.txt"
    flatpak "${args[@]}" >"$logdir/mixxx.log" 2>&1 &
    launcher=$!

    pid=
    for _ in $(seq 1 60); do
        pid=$(pgrep -n -x mixxx.bin) && break
        kill -0 "$launcher" 2>/dev/null || break
        sleep 0.5
    done
    [ -n "$pid" ] || { tail -n 40 "$logdir/mixxx.log"; die "Mixxx did not start"; }

    stop_mixxx() {
        # bubblewrap does not forward signals: terminate mixxx.bin itself (graceful quit).
        kill -TERM "$pid" 2>/dev/null
        for _ in $(seq 1 30); do kill -0 "$pid" 2>/dev/null || break; sleep 0.5; done
        kill -KILL "$pid" 2>/dev/null
        wait "$launcher" 2>/dev/null
    }
    trap 'stop_mixxx; exit 130' INT TERM

    note "running for ${seconds}s (pid $pid). Leave the decks paused for an idle sample, then play."
    {
        echo "# $(date -Is) $ref pid $pid, ${sample}s samples. CPU% is of one core."
        echo "# elapsed  total%  v3d_irq/s  vc4_irq/s  top threads (comm:tid=cpu%)"
    } >"$logdir/cpu.log"
    declare -A last
    while read -r tid comm ticks; do last[$tid]=$ticks; done < <(sample_threads "$pid")
    v3d_last=$(irq_count 'v3d')
    vc4_last=$(irq_count 'vc4')
    start=$(date +%s)
    while kill -0 "$pid" 2>/dev/null; do
        sleep "$sample"
        kill -0 "$pid" 2>/dev/null || break
        elapsed=$(($(date +%s) - start))
        lines=()
        total=0
        while read -r tid comm ticks; do
            prev=${last[$tid]:-$ticks}
            delta=$((ticks - prev))
            last[$tid]=$ticks
            total=$((total + delta))
            [ "$delta" -gt 0 ] && lines+=("$delta $comm:$tid")
        done < <(sample_threads "$pid")
        v3d=$(irq_count 'v3d'); vc4=$(irq_count 'vc4')
        top=$(printf '%s\n' "${lines[@]}" | sort -rn | head -n 6 |
            awk -v hz="$hz" -v s="$sample" '{ printf "%s=%.1f ", $2, 100 * $1 / (hz * s) }')
        printf '%6ss  %6.1f  %9.0f  %9.0f  %s\n' "$elapsed" \
            "$(awk -v t="$total" -v hz="$hz" -v s="$sample" 'BEGIN { print 100 * t / (hz * s) }')" \
            "$(((v3d - v3d_last) / sample))" "$(((vc4 - vc4_last) / sample))" "$top" | tee -a "$logdir/cpu.log"
        v3d_last=$v3d; vc4_last=$vc4
        grep 'DECKLAB_QML_PERF window' "$logdir/mixxx.log" | tail -n 1 | sed 's/^.*DECKLAB_QML_PERF/  fps:/' >>"$logdir/cpu.log"
        [ "$elapsed" -ge "$seconds" ] && break
    done
    stop_mixxx
    trap - INT TERM
    {
        echo "# Qt scene graph and GL:"
        grep -E 'qt.scenegraph.general|threaded|basic render loop|GL_RENDERER|DECKLAB_QML_PERF gl' "$logdir/mixxx.log" | head -n 12
        echo "# frames per 10 s:"
        grep 'DECKLAB_QML_PERF window' "$logdir/mixxx.log"
    } >>"$logdir/cpu.log"
    note "done. Summary: $logdir/cpu.log  Full log: $logdir/mixxx.log"
    note "the kiosk is still stopped. Revert with: $0 restore-kiosk   (or reboot)"
}

case ${1:-} in
check) cmd_check ;;
stop-kiosk) cmd_stop_kiosk ;;
restore-kiosk) cmd_restore_kiosk ;;
run) shift; cmd_run "$@" ;;
*) sed -n '2,13p' "$0"; exit 2 ;;
esac

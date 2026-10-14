#!/usr/bin/env bash
# 采样网关进程的 CPU / RSS / 文件描述符 / 帧计数与队列指标，用于改造前后同参数对比。
# 只读 /proc 与 /api/status，不修改日常配置；录像与日志留在本次的 data/measure-XXXXXX 目录。
#
# 用法: bash scripts/measure-run.sh [采样秒数] [摄像头设备]
#   例: bash scripts/measure-run.sh 60
#       bash scripts/measure-run.sh 60 /dev/v4l/by-id/usb-xxx-video-index0
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.."

duration=${1:-60}
video_device=${2:-/dev/video11}
port=18081

for tool in curl awk getconf; do
    command -v "$tool" >/dev/null || { printf 'Missing tool: %s\n' "$tool" >&2; exit 1; }
done
[[ -x build-native/edgefusion_gateway ]] || { printf '%s\n' 'Build first: make -f Makefile.native -j2' >&2; exit 1; }

mkdir -p data
run_dir=$(mktemp -d "$PWD/data/measure-XXXXXX")
mkdir -p "$run_dir/recordings" "$run_dir/snapshots"
csv="$run_dir/samples.csv"
pid=
stop_rc=

cat > "$run_dir/gateway.conf" <<EOF
source_type = v4l2
video_device = $video_device
capture_width = 640
capture_height = 480
capture_fps = 30
video_bitrate_kbps = 1500
rtsp_timeout_us = 5000000
rtsp_reconnect_s = 3
rtsp_server_enable = 0
storage_dir = $run_dir/recordings
segment_seconds = 60
storage_min_free_mb = 500
sqlite_db_path = $run_dir/gateway.db
web_bind = 127.0.0.1
web_port = $port
web_root = web
web_mjpeg_fps = 5
ai_enable = 0
alarm_gpio_led = -1
alarm_gpio_buzzer = -1
log_level = info
EOF

stop_gateway() {
    [[ -n "$pid" ]] || return 0
    kill -0 "$pid" 2>/dev/null || { wait "$pid" 2>/dev/null || true; pid=; return 0; }
    local t0=$SECONDS
    kill -INT "$pid" 2>/dev/null || true
    for ((i=0; i<50; i++)); do kill -0 "$pid" 2>/dev/null || break; sleep 0.1; done
    if kill -0 "$pid" 2>/dev/null; then
        printf 'FAIL: gateway did not stop within 5 seconds\n' >&2
        kill -KILL "$pid" 2>/dev/null || true
        wait "$pid" 2>/dev/null || true
        pid=
        stop_rc=1
        return 1
    fi
    wait "$pid" 2>/dev/null || true
    pid=
    printf 'Exit latency: %d s\n' $((SECONDS - t0))
    return 0
}

on_exit() {
    local rc=$?
    trap - EXIT
    if ! stop_gateway; then rc=1; fi
    printf 'Artifacts: %s\n' "$run_dir"
    exit "$rc"
}
trap on_exit EXIT
trap 'exit 130' INT TERM

printf 'Measuring for %s s with %s ...\n' "$duration" "$video_device"
./build-native/edgefusion_gateway -path "$run_dir/gateway.conf" > "$run_dir/gateway.log" 2>&1 &
pid=$!
sleep 3
kill -0 "$pid" 2>/dev/null || { printf 'FAIL: gateway exited during startup\n' >&2; tail -n 40 "$run_dir/gateway.log" >&2; exit 1; }

clk=$(getconf CLK_TCK)
printf 'sec,cpu_pct,rss_kb,fds,frames,frames_per_s,queue_depth,queue_dropped,preview_processed\n' > "$csv"

prev_ticks=$(awk '{print $14+$15}' "/proc/$pid/stat")
prev_frames=0
for ((s=1; s<=duration; s++)); do
    sleep 1
    if ! kill -0 "$pid" 2>/dev/null; then
        printf 'FAIL: gateway exited while sampling\n' >&2
        tail -n 40 "$run_dir/gateway.log" >&2
        exit 1
    fi
    ticks=$(awk '{print $14+$15}' "/proc/$pid/stat")
    cpu=$(awk -v d="$((ticks-prev_ticks))" -v hz="$clk" 'BEGIN{printf "%.1f", d/hz*100}')
    prev_ticks=$ticks
    rss=$(awk '/VmRSS/{print $2}' "/proc/$pid/status" || true); rss=${rss:-0}
    fds=$(ls "/proc/$pid/fd" 2>/dev/null | wc -l || true); fds=${fds:-0}

    json=$(curl -s --max-time 2 "http://127.0.0.1:$port/api/status" || true)
    frames=$(printf '%s' "$json" | sed -n 's/.*"frames":\([0-9][0-9]*\).*/\1/p'); frames=${frames:-0}
    depth=$(printf '%s' "$json" | sed -n 's/.*"depth":\([0-9][0-9]*\).*/\1/p'); depth=${depth:-0}
    dropped=$(printf '%s' "$json" | sed -n 's/.*"dropped":\([0-9][0-9]*\).*/\1/p'); dropped=${dropped:-0}
    processed=$(printf '%s' "$json" | sed -n 's/.*"processed":\([0-9][0-9]*\).*/\1/p'); processed=${processed:-0}
    delta=$((frames - prev_frames))
    prev_frames=$frames

    printf '%d,%s,%s,%s,%s,%d,%s,%s,%s\n' \
        "$s" "$cpu" "${rss:-0}" "$fds" "$frames" "$delta" "$depth" "$dropped" "$processed" >> "$csv"
    printf '\r[%3d/%3d] cpu=%5s%% rss=%7s kB fds=%s frames=%s (+%d/s) queue=%s drop=%s' \
        "$s" "$duration" "$cpu" "${rss:-0}" "$fds" "$frames" "$delta" "$depth" "$dropped"
done
printf '\n'

stop_gateway

# 汇总：均值/峰值 CPU、RSS 变化、平均帧率、累计丢包
awk -F, '
NR > 1 {
    n++; cpu += $2; if ($2 > maxcpu) maxcpu = $2;
    if (n == 1) rss_first = $3;
    rss_last = $3; if ($3 > maxrss) maxrss = $3;
    fps += $6; frames = $5; dropped = $8; processed = $9;
    if ($4 > maxfds) maxfds = $4;
}
END {
    if (n == 0) { print "no samples"; exit 1 }
    printf "samples            : %d\n", n;
    printf "cpu avg / max      : %.1f%% / %.1f%%\n", cpu / n, maxcpu;
    printf "rss first / last   : %d kB / %d kB (peak %d kB)\n", rss_first, rss_last, maxrss;
    printf "fds max            : %d\n", maxfds;
    printf "frames total       : %d\n", frames;
    printf "frames_per_s avg   : %.2f\n", fps / n;
    printf "preview dropped    : %d\n", dropped;
    printf "preview processed  : %d\n", processed;
}' "$csv" | tee "$run_dir/summary.txt"

# 采样期间出现的像素格式告警也必须为 0（与 smoke-usb.sh 同一验收条件）
warnings=$(grep -c 'deprecated pixel format' "$run_dir/gateway.log" || true)
warnings=${warnings:-0}
(( warnings == 0 )) || { printf 'FAIL: %s deprecated pixel format warnings\n' "$warnings" >&2; exit 1; }
printf 'PASS: measurement complete, no deprecated pixel format warnings\n'

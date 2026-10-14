#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.."
for tool in curl ffprobe ffmpeg python3; do
    command -v "$tool" >/dev/null || { printf 'Missing tool: %s\n' "$tool" >&2; exit 1; }
done
[[ -x build-native/edgefusion_gateway ]] || { printf '%s\n' 'Build first: make -f Makefile.native -j2'; exit 1; }
mkdir -p data
run_dir=$(mktemp -d "$PWD/data/smoke-XXXXXX")
mkdir -p "$run_dir/recordings" "$run_dir/snapshots"
video_device=${1:-/dev/video11}
pid=
stop_gateway() {
    if [[ -z "$pid" ]]; then return; fi
    if kill -0 "$pid" 2>/dev/null; then
        kill -INT "$pid" 2>/dev/null || true
        for ((i=0; i<50; i++)); do
            if ! kill -0 "$pid" 2>/dev/null; then break; fi
            sleep 0.1
        done
        if kill -0 "$pid" 2>/dev/null; then
            printf '%s\n' 'FAIL: gateway did not stop within 5 seconds' >&2
            kill -KILL "$pid" 2>/dev/null || true
            wait "$pid" 2>/dev/null || true
            pid=
            return 1
        fi
    fi
    local result=0
    wait "$pid" || result=$?
    pid=
    return "$result"
}
on_exit() {
    local result=$?
    trap - EXIT
    if ! stop_gateway; then result=1; fi
    if (( result != 0 )); then tail -n 60 "$run_dir/gateway.log" 2>/dev/null || true; fi
    printf 'Artifacts and log: %s\n' "$run_dir"
    exit "$result"
}
trap on_exit EXIT
trap 'exit 130' INT TERM
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
segment_seconds = 3
storage_min_free_mb = 500
sqlite_db_path = $run_dir/gateway.db
web_bind = 127.0.0.1
web_port = 18080
web_root = web
web_mjpeg_fps = 5
ai_enable = 0
alarm_gpio_led = -1
alarm_gpio_buzzer = -1
log_level = info
EOF
./build-native/edgefusion_gateway -path "$run_dir/gateway.conf" > "$run_dir/gateway.log" 2>&1 &
pid=$!
sleep 3
kill -0 "$pid"
curl --max-time 3 -fsS http://127.0.0.1:18080/api/status > "$run_dir/status-before.json"
sleep 8
curl --max-time 3 -fsS http://127.0.0.1:18080/api/status > "$run_dir/status-after.json"
python3 - "$run_dir" <<'PY'
import json, pathlib, sys
p = pathlib.Path(sys.argv[1])
a = json.loads((p / 'status-before.json').read_text())['stream']
b = json.loads((p / 'status-after.json').read_text())['stream']
assert a['source_type'] == b['source_type'] == 'v4l2', (a, b)
assert a['connected'] and b['connected'] and b['recording'], (a, b)
assert b['frames'] > a['frames'] > 0, (a, b)
assert b['reconnects'] == a['reconnects'] == 0, (a, b)
print('PASS: USB connected, recording, increasing packet count; no retries')
# preview_queue 只存在于“预览/转发已下到独立线程”的版本：老版本跳过，
# 新版本必须证明该线程确实在消费，且正常负载下不丢包。
pq = b.get('preview_queue')
if pq is not None:
    assert pq['processed'] > 0, pq
    assert pq['dropped'] == 0, pq
    print('PASS: async preview consumed %d packets, dropped %d'
          % (pq['processed'], pq['dropped']))
# 只在“预览直通相机 MJPEG”的版本里断言：USB 模式必须是 true
if 'preview_raw_jpeg' in b:
    assert b['preview_raw_jpeg'] is True, b['preview_raw_jpeg']
    print('PASS: preview fed from camera MJPEG (no H.264 re-decode)')
print(json.dumps(b, ensure_ascii=False))
PY
stop_gateway
shopt -s nullglob
segments=("$run_dir"/recordings/*.mp4)
(( ${#segments[@]} >= 2 )) || { printf '%s\n' 'FAIL: expected at least two segments'; exit 1; }
for segment in "${segments[@]}"; do
    ffprobe -v error -select_streams v:0 -show_entries stream=codec_name,width,height -show_entries format=duration -of json "$segment"
    ffmpeg -v error -xerror -i "$segment" -map 0:v:0 -f null -
done
# image_scale.h 修复的验收条件：swscale 不再打印 YUVJ 弃用告警
warnings=$(grep -c 'deprecated pixel format' "$run_dir/gateway.log" || true)
warnings=${warnings:-0}
(( warnings == 0 )) || { printf 'FAIL: %s deprecated pixel format warnings in gateway.log\n' "$warnings" >&2; exit 1; }
printf 'PASS: no deprecated pixel format warnings (%s found)\n' "$warnings"
printf '%s\n' 'PASS: USB -> H.264 -> MP4, HTTP status, segment decode and bounded shutdown'

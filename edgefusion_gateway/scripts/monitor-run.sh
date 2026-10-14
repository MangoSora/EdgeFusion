#!/usr/bin/env bash
# 长期运行采样：对**已在运行**的网关进程按秒采样 CPU/RSS/文件描述符/帧率/队列指标，
# 直到 Ctrl+C 或进程退出。与 measure-run.sh 的区别：它不启动也不停止网关，
# 因此可以挂在 24–72 小时稳定性测试旁边。
#
#   bash scripts/monitor-run.sh                       # 1 秒间隔，输出到 data/monitor-<时间戳>/
#   bash scripts/monitor-run.sh 5                     # 5 秒间隔
#   bash scripts/monitor-run.sh 5 /path/to/outdir
#   bash scripts/monitor-run.sh 5 data/monitor-x http://127.0.0.1:8080/api/status
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.."

interval=${1:-1}
out_dir=${2:-data/monitor-$(date +%Y%m%d_%H%M%S)}
status_url=${3:-http://127.0.0.1:8080/api/status}

for tool in curl awk getconf pgrep; do
    command -v "$tool" >/dev/null || { printf '缺少命令: %s\n' "$tool" >&2; exit 1; }
done

pid=$(pgrep -n -f 'build-native/edgefusion_gateway' || true)
[[ -n "$pid" ]] || pid=$(pgrep -n -x edgefusion_gate || true)
[[ -n "$pid" ]] || {
    printf '%s\n' '未找到运行中的 edgefusion_gateway（可用 systemctl start edgefusion-gateway 启动）' >&2
    exit 1; }

mkdir -p "$out_dir"
csv="$out_dir/samples.csv"
printf 'time,cpu_pct,rss_kb,fds,frames,frames_per_s,queue_depth,queue_dropped,preview_processed,reconnects\n' > "$csv"
printf '采样 pid=%s，间隔 %ss，输出 %s（Ctrl+C 结束）\n' "$pid" "$interval" "$csv"

clk=$(getconf CLK_TCK)
prev_ticks=$(awk '{print $14+$15}' "/proc/$pid/stat")
prev_frames=0
n=0

on_int() {
    printf '\n已停止采样：%s（%d 条样本）\n' "$csv" "$n"
    exit 0
}
trap on_int INT TERM

while kill -0 "$pid" 2>/dev/null; do
    sleep "$interval"
    kill -0 "$pid" 2>/dev/null || break
    ticks=$(awk '{print $14+$15}' "/proc/$pid/stat" 2>/dev/null || echo "$prev_ticks")
    cpu=$(awk -v d="$((ticks-prev_ticks))" -v hz="$clk" -v iv="$interval" \
          'BEGIN{printf "%.1f", d/hz/iv*100}')
    prev_ticks=$ticks
    rss=$(awk '/VmRSS/{print $2}' "/proc/$pid/status" 2>/dev/null || true); rss=${rss:-0}
    fds=$(ls "/proc/$pid/fd" 2>/dev/null | wc -l || true); fds=${fds:-0}

    json=$(curl -s --max-time 3 "$status_url" || true)
    frames=$(printf '%s' "$json" | sed -n 's/.*"frames":\([0-9][0-9]*\).*/\1/p'); frames=${frames:-0}
    depth=$(printf '%s' "$json" | sed -n 's/.*"depth":\([0-9][0-9]*\).*/\1/p'); depth=${depth:-0}
    dropped=$(printf '%s' "$json" | sed -n 's/.*"dropped":\([0-9][0-9]*\).*/\1/p'); dropped=${dropped:-0}
    processed=$(printf '%s' "$json" | sed -n 's/.*"processed":\([0-9][0-9]*\).*/\1/p'); processed=${processed:-0}
    reconn=$(printf '%s' "$json" | sed -n 's/.*"reconnects":\([0-9][0-9]*\).*/\1/p'); reconn=${reconn:-0}
    delta=$((frames - prev_frames))
    prev_frames=$frames

    printf '%s,%s,%s,%s,%s,%d,%s,%s,%s,%s\n' \
        "$(date +%H:%M:%S)" "$cpu" "$rss" "$fds" "$frames" "$delta" "$depth" "$dropped" "$processed" "$reconn" \
        >> "$csv"
    n=$((n + 1))
    if (( n % 10 == 0 )); then
        printf '[%d] cpu=%s%% rss=%s kB fds=%s frames=%s drop=%s\n' \
            "$n" "$cpu" "$rss" "$fds" "$frames" "$dropped"
    fi
done

printf '进程已退出（pid=%s），共 %d 条样本：%s\n' "$pid" "$n" "$csv"
awk -F, '
NR > 1 {
    m++; cpu += $2; if ($2 > maxcpu) maxcpu = $2;
    if (m == 1) { rss_first = $3; fd_first = $4 }
    rss_last = $3; fd_last = $4; if ($3 > maxrss) maxrss = $3; if ($4 > maxfd) maxfd = $4;
    fps += $6; frames = $5; dropped = $8; processed = $9; reconn = $10
}
END {
    if (m == 0) { print "无样本"; exit 0 }
    printf "样本数            : %d\n", m;
    printf "CPU 均值 / 峰值   : %.1f%% / %.1f%%\n", cpu / m, maxcpu;
    printf "RSS 首 / 末 / 峰  : %d / %d / %d kB\n", rss_first, rss_last, maxrss;
    printf "FD  首 / 末 / 峰  : %d / %d / %d\n", fd_first, fd_last, maxfd;
    printf "平均帧率          : %.2f 包/秒\n", fps / m;
    printf "累计帧数          : %d\n", frames;
    printf "预览丢包 / 已处理 : %d / %d\n", dropped, processed;
    printf "累计重连次数      : %d\n", reconn;
}' "$csv" | tee "$out_dir/summary.txt"

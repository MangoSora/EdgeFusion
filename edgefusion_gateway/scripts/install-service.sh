#!/usr/bin/env bash
# 安装/更新/卸载 EdgeFusion Gateway 的 systemd 服务（板端执行，中途会要 sudo 口令）。
#
#   bash scripts/install-service.sh              # 安装并立即启动（幂等，可重复执行）
#   bash scripts/install-service.sh --uninstall   # 停止并移除服务（不动工程文件与录像）
#
# 安装内容：把 deploy/edgefusion-gateway.service 里的 @USER@ / @DIR@ 替换成
# 当前用户与本仓库绝对路径后写入 /etc/systemd/system/。
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.."

unit_src="deploy/edgefusion-gateway.service"
unit_dst="/etc/systemd/system/edgefusion-gateway.service"
service_name="edgefusion-gateway"

if [[ "${1:-}" == "--uninstall" ]]; then
    sudo systemctl disable --now "$service_name" 2>/dev/null || true
    sudo rm -f "$unit_dst"
    sudo systemctl daemon-reload
    printf '%s\n' "已卸载 $service_name 服务（工程文件、录像与数据目录未改动）"
    exit 0
fi

[[ -f "$unit_src" ]] || { printf '缺少服务单元: %s\n' "$unit_src" >&2; exit 1; }
[[ -x build-native/edgefusion_gateway ]] || {
    printf '%s\n' '还没编译：先执行 make -f Makefile.native -j2' >&2; exit 1; }
[[ -f config/rk3576-usb.conf ]] || {
    printf '缺少配置: %s\n' config/rk3576-usb.conf >&2; exit 1; }

repo_dir=$(pwd)
run_user=${SUDO_USER:-${USER:-$(id -un)}}

printf '仓库路径: %s\n运行用户: %s\n' "$repo_dir" "$run_user"
sed -e "s|@USER@|$run_user|g" -e "s|@DIR@|$repo_dir|g" "$unit_src" \
    | sudo tee "$unit_dst" >/dev/null

sudo systemctl daemon-reload
sudo systemctl enable --now "$service_name"
sleep 2
systemctl --no-pager --full status "$service_name" || true

printf '\n常用命令:\n'
printf '  看日志    : journalctl -u %s -f\n' "$service_name"
printf '  重启      : sudo systemctl restart %s\n' "$service_name"
printf '  停止      : sudo systemctl stop %s\n' "$service_name"
printf '  查状态接口: curl -s http://127.0.0.1:8080/api/status\n'
printf '  长期采样  : bash scripts/monitor-run.sh\n'
printf '  卸载      : bash scripts/install-service.sh --uninstall\n'

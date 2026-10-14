#!/usr/bin/env bash
# MQTT 断线重连验证：用户态 broker(1884) → 客户端连上 → 杀 broker → 重启 broker → 看是否自动恢复
set -u
cd "$HOME/Program/EdgeFusion_Smart_NVR/edgefusion_gateway"

CONF=/tmp/mqtt-broker1884.conf
LOG=/tmp/mqtt-reconnect.log

cat > "$CONF" <<'EOF'
listener 1884 127.0.0.1
allow_anonymous true
persistence false
EOF

echo "=== 0) 编译重连测试程序 ==="
gcc -O2 -g -Wall -Wextra -Isrc -D_GNU_SOURCE \
    tests/test_mqtt_reconnect.c src/mqtt_client.c src/log.c -pthread \
    -o build-native/test_mqtt_reconnect 2>&1 | head -20
ls -l build-native/test_mqtt_reconnect

echo
echo "=== 1) 启动用户态 broker(1884) ==="
pkill -f "$CONF" 2>/dev/null; sleep 0.5
mosquitto -c "$CONF" -d
sleep 1
ss -ltn | grep 1884 || { echo "broker 未监听 1884"; exit 1; }

echo
echo "=== 2) 启动客户端（后台，共 26 秒）==="
build-native/test_mqtt_reconnect 1884 26 > "$LOG" 2>&1 &
CL=$!
sleep 7
echo "--- 杀掉前 3 行 ---"; head -3 "$LOG"

echo
echo "=== 3) 杀掉 broker（模拟断线）==="
date +"kill at %T"
pkill -f "$CONF"
sleep 9

echo "=== 4) 重启 broker ==="
date +"restart at %T"
mosquitto -c "$CONF" -d
sleep 1
ss -ltn | grep 1884 >/dev/null && echo "broker 已恢复监听"

echo
echo "=== 5) 等客户端跑完 ==="
wait "$CL"; rc=$?
echo "client_exit_rc=$rc"

echo
echo "=== 6) 客户端日志（关键事件）==="
grep -nE "已连接|连接断开|重试|心跳超时|重发|PASS|FAIL|FINAL|^t=" "$LOG" | head -40

echo
echo "=== 7) 客户端日志（完整尾部）==="
tail -12 "$LOG"

echo
echo "=== 8) 清理 ==="
pkill -f "$CONF" 2>/dev/null
echo "=== MQTT_RECONNECT_TEST_DONE ==="

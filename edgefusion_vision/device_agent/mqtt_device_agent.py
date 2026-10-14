#!/usr/bin/env python3
"""EdgeFusion 端侧代理（device side agent）

角色：模拟一台"端侧网络摄像机"。持有摄像头，负责
  1) 周期上报状态（心跳），掉线由 MQTT 遗嘱消息(LWT)自动置为 offline；
  2) 帧差法做移动检测，产生事件；
  3) 按间隔或收到命令时上传 JPEG 快照；
  4) 订阅命令主题，执行抓拍 / 改间隔 / 改灵敏度 / 暂停恢复。

与网关（边）的约定：
  上行  edgefusion/device/<id>/status     JSON  保留消息，心跳 + 遗嘱
        edgefusion/device/<id>/event      JSON  QoS1 事件
        edgefusion/device/<id>/snapshot   JPEG  QoS0 快照
  下行  edgefusion/device/<id>/cmd        JSON  命令

用法（PC 上运行）：
    python mqtt_device_agent.py --broker 192.168.137.2 --device A1
    python mqtt_device_agent.py --broker 192.168.137.2 --camera 0 --snapshot-interval 3
"""
import argparse
import json
import sys
import time

import cv2
import numpy as np
import paho.mqtt.client as mqtt


class DeviceAgent:
    def __init__(self, args):
        self.args = args
        self.prefix = f"edgefusion/device/{args.device}"
        self.running = True
        self.paused = False
        self.snapshot_interval = args.snapshot_interval
        self.motion_ratio = args.motion_ratio          # 触发事件的像素占比阈值
        self.last_event = 0.0
        self.stats = {"snapshots": 0, "events": 0, "cmds": 0}

        self.client = mqtt.Client(
            mqtt.CallbackAPIVersion.VERSION2,
            client_id=f"device-{args.device}",
            protocol=mqtt.MQTTv311,
        )
        # 遗嘱：代理异常退出时，broker 自动替我们发布 offline
        self.client.will_set(
            f"{self.prefix}/status",
            json.dumps({"online": False, "device": args.device}),
            qos=1, retain=True,
        )
        self.client.on_connect = self.on_connect
        self.client.on_message = self.on_message
        self.client.on_disconnect = self.on_disconnect

        self.cap = cv2.VideoCapture(args.camera)
        if not self.cap.isOpened():
            print(f"[agent] 打不开摄像头 {args.camera}", file=sys.stderr)
            sys.exit(2)
        self.cap.set(cv2.CAP_PROP_FRAME_WIDTH, args.width)
        self.cap.set(cv2.CAP_PROP_FRAME_HEIGHT, args.height)
        self.background = None

    # ---------- MQTT 回调 ----------
    def on_connect(self, client, userdata, flags, reason_code, properties=None):
        print(f"[agent] 已连接 broker（rc={reason_code}），订阅命令主题")
        client.subscribe(f"{self.prefix}/cmd", qos=1)
        self.publish_status(online=True)

    def on_disconnect(self, client, userdata, flags, reason_code, properties=None):
        print(f"[agent] 与 broker 断开（rc={reason_code}），paho 会自动重连")

    def on_message(self, client, userdata, msg):
        try:
            cmd = json.loads(msg.payload.decode("utf-8", "ignore"))
        except Exception as exc:                                  # noqa: BLE001
            print(f"[agent] 命令解析失败: {exc}")
            return
        name = cmd.get("cmd", "")
        self.stats["cmds"] += 1
        print(f"[agent] 收到命令: {cmd}")
        if name == "snapshot":
            self.publish_snapshot(reason="cmd")
        elif name == "set_snapshot_interval":
            self.snapshot_interval = max(1, int(cmd.get("value", self.snapshot_interval)))
        elif name == "set_motion_ratio":
            self.motion_ratio = float(cmd.get("value", self.motion_ratio))
        elif name == "pause":
            self.paused = True
        elif name == "resume":
            self.paused = False
        elif name == "stop":
            self.running = False
        else:
            print(f"[agent] 未知命令: {name}")

    # ---------- 发布 ----------
    def publish_status(self, online=True):
        payload = {
            "online": online,
            "device": self.args.device,
            "ts": time.time(),
            "width": int(self.cap.get(cv2.CAP_PROP_FRAME_WIDTH)),
            "height": int(self.cap.get(cv2.CAP_PROP_FRAME_HEIGHT)),
            "fps": self.args.report_fps,
            "paused": self.paused,
            **self.stats,
        }
        self.client.publish(f"{self.prefix}/status", json.dumps(payload), qos=0, retain=True)

    def publish_snapshot(self, reason="interval"):
        ok, frame = self.cap.read()
        if not ok:
            print("[agent] 读帧失败")
            return
        if frame.shape[1] != self.args.snapshot_width:
            scale = self.args.snapshot_width / frame.shape[1]
            frame = cv2.resize(frame, (self.args.snapshot_width,
                                       int(frame.shape[0] * scale)))
        ok, jpg = cv2.imencode(".jpg", frame, [cv2.IMWRITE_JPEG_QUALITY, 70])
        if not ok:
            return
        data = jpg.tobytes()
        self.client.publish(f"{self.prefix}/snapshot", data, qos=0, retain=False)
        self.stats["snapshots"] += 1
        print(f"[agent] 快照 {len(data)} 字节（{reason}）")

    def publish_event(self, kind, confidence, size):
        payload = {
            "device": self.args.device,
            "ts": time.time(),
            "event": kind,
            "confidence": round(confidence, 3),
            "width": size[0],
            "height": size[1],
        }
        self.client.publish(f"{self.prefix}/event", json.dumps(payload), qos=1)
        self.stats["events"] += 1
        print(f"[agent] 事件 {kind} conf={confidence:.2f}")

    # ---------- 主循环 ----------
    def run(self):
        self.client.connect(self.args.broker, self.args.port, keepalive=15)
        self.client.loop_start()

        t_status = t_snapshot = time.time()
        frames = 0
        t_fps = time.time()
        try:
            while self.running:
                ok, frame = self.cap.read()
                if not ok:
                    time.sleep(0.2)
                    continue
                frames += 1

                if not self.paused:
                    small = cv2.cvtColor(cv2.resize(frame, (160, 120)), cv2.COLOR_BGR2GRAY)
                    small = cv2.GaussianBlur(small, (5, 5), 0)
                    if self.background is None:
                        self.background = small
                    else:
                        diff = cv2.absdiff(small, self.background)
                        moving = int(np.count_nonzero(diff > 25))
                        ratio = moving / float(small.size)
                        # 背景缓慢更新：不用 accumulateWeighted——OpenCV 5 已不支持 uint8 的类型组合
                        self.background = cv2.addWeighted(self.background, 0.95, small, 0.05, 0.0)
                        now = time.time()
                        if ratio >= self.motion_ratio and now - self.last_event > self.args.event_cooldown:
                            self.last_event = now
                            self.publish_event("motion", min(1.0, ratio * 8), small.shape[::-1])

                now = time.time()
                if now - t_status >= self.args.status_interval:
                    t_status = now
                    self.publish_status(online=True)
                    self.args.report_fps = round(frames / max(1e-6, now - t_fps), 1)
                    frames, t_fps = 0, now
                if not self.paused and now - t_snapshot >= self.snapshot_interval:
                    t_snapshot = now
                    self.publish_snapshot(reason="interval")

                time.sleep(0.01)
        except KeyboardInterrupt:
            print("\n[agent] 收到 Ctrl+C，准备退出")
        finally:
            self.publish_status(online=False)
            time.sleep(0.3)                       # 给 offline 消息一点发送时间
            self.client.loop_stop()
            self.client.disconnect()
            self.cap.release()
            print(f"[agent] 已退出，统计: {self.stats}")


def main():
    p = argparse.ArgumentParser(description="EdgeFusion 端侧代理（MQTT 设备模拟）")
    p.add_argument("--broker", default="192.168.137.2", help="broker 地址（默认板子）")
    p.add_argument("--port", type=int, default=1883)
    p.add_argument("--device", default="A1", help="设备 ID（主题里用）")
    p.add_argument("--camera", type=int, default=0, help="摄像头序号")
    p.add_argument("--width", type=int, default=1280)
    p.add_argument("--height", type=int, default=720)
    p.add_argument("--snapshot-width", type=int, default=640)
    p.add_argument("--snapshot-interval", type=float, default=5.0, help="快照间隔秒")
    p.add_argument("--status-interval", type=float, default=5.0, help="心跳间隔秒")
    p.add_argument("--motion-ratio", type=float, default=0.01, help="移动检测阈值(像素占比)")
    p.add_argument("--event-cooldown", type=float, default=5.0, help="同类事件冷却秒")
    p.add_argument("--report-fps", type=float, default=0.0)
    DeviceAgent(p.parse_args()).run()


if __name__ == "__main__":
    main()

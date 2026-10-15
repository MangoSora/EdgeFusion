#!/usr/bin/env python3
"""RTSP 拉流验证工具：从网关的 RTSP 转发端口拉流，统计帧数，可选弹窗显示。

作用：它就是一个**普通 RTSP 客户端**（和 VLC 等价），用来验证
  1) 网关的 RTSP 转发是否工作；
  2) 多个客户端能否同时拉同一路流（程序里多开几个即可）。

用法（在你 PC 上）：
    python rtsp-pull-test.py rtsp://192.168.137.2:8554/track0 --seconds 15
    python rtsp-pull-test.py rtsp://192.168.137.2:8554/track0 --seconds 15 --show

依赖：opencv-python（自带 FFmpeg 后端，能直接解 RTSP/H.264），无需安装 VLC。
"""
import argparse
import os
import sys
import time

import cv2


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("url", help="例 rtsp://192.168.137.2:8554/track0")
    ap.add_argument("--seconds", type=float, default=15.0, help="拉流时长")
    ap.add_argument("--show", action="store_true", help="弹窗显示画面（按 q 提前退出）")
    args = ap.parse_args()

    # Windows 上默认强制 RTSP over TCP：UDP 在部分网络/网卡驱动下会丢包花屏
    if os.name == "nt" and "OPENCV_FFMPEG_CAPTURE_OPTIONS" not in os.environ:
        os.environ["OPENCV_FFMPEG_CAPTURE_OPTIONS"] = "rtsp_transport;tcp"
        print("[info] 已默认强制 RTSP/TCP（想用 UDP 请先清空 OPENCV_FFMPEG_CAPTURE_OPTIONS）")

    cap = cv2.VideoCapture(args.url, cv2.CAP_FFMPEG)
    if not cap.isOpened():
        print(f"[FAIL] 打不开 {args.url}")
        print("       检查：网关是否以 rtsp_server_enable=1 启动、8554 是否在监听、URL 是否正确")
        return 2

    w = int(cap.get(cv2.CAP_PROP_FRAME_WIDTH))
    h = int(cap.get(cv2.CAP_PROP_FRAME_HEIGHT))
    print(f"[ok] 已连接：{w}x{h}，开始拉流 {args.seconds:.0f} 秒 ...")

    n = 0
    t0 = time.time()
    last = t0
    while time.time() - t0 < args.seconds:
        ok, frame = cap.read()
        if not ok:
            print("[warn] 读帧失败（流被中断？）")
            break
        n += 1
        if args.show:
            cv2.imshow("RTSP pull test  (press q to quit)", frame)
            if (cv2.waitKey(1) & 0xFF) == ord("q"):
                break
        now = time.time()
        if now - last >= 3.0:
            last = now
            print(f"  已拉 {n} 帧，平均 {n / max(now - t0, 1e-6):.1f} fps")
    dt = max(time.time() - t0, 1e-6)

    cap.release()
    if args.show:
        cv2.destroyAllWindows()
    print(f"[done] 共 {n} 帧 / {dt:.1f} 秒 = {n / dt:.1f} fps")
    return 0 if n > 0 else 1


if __name__ == "__main__":
    sys.exit(main())

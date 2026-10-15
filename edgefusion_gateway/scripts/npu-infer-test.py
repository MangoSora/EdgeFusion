#!/usr/bin/env python3
"""板端 NPU 推理验证工具（RKNN + rknnlite）。

用途：
  1) 验证 RK3576 NPU 能加载模型并推理（延迟/吞吐）；
  2) 对真实场景图片做检测（YOLOv5 anchor-based 解码 + NMS），输出类别与框；
  3) 为后续"NPU 作为分析后端"（与云端 AI 可插拔）打基础。

用法（板子上，用带 rknnlite 的 venv）：
  ~/venvs/rknn-lite2/bin/python scripts/npu-infer-test.py \
      --model <某.rknn> --images ~/Program/EdgeFusion_Smart_NVR/edgefusion_gateway/data/remote/A1 \
      --num 8 --runs 20

注意：默认按 YOLOv5 (1,255,80,80)/(1,255,40,40)/(1,255,20,20) 三尺度解码；
     换成 YOLOv8/YOLO11 时输出是 (1,84,N) 无 objectness，需要另一套解码（本脚本会提示）。
"""
import argparse
import glob
import os
import random
import statistics
import sys
import time

import cv2
import numpy as np
from rknnlite.api import RKNNLite

COCO80 = ["person", "bicycle", "car", "motorcycle", "airplane", "bus", "train", "truck", "boat",
          "traffic light", "fire hydrant", "stop sign", "parking meter", "bench", "bird", "cat",
          "dog", "horse", "sheep", "cow", "elephant", "bear", "zebra", "giraffe", "backpack",
          "umbrella", "handbag", "tie", "suitcase", "frisbee", "skis", "snowboard", "sports ball",
          "kite", "baseball bat", "baseball glove", "skateboard", "surfboard", "tennis racket",
          "bottle", "wine glass", "cup", "fork", "knife", "spoon", "bowl", "banana", "apple",
          "sandwich", "orange", "broccoli", "carrot", "hot dog", "pizza", "donut", "cake", "chair",
          "couch", "potted plant", "bed", "dining table", "toilet", "tv", "laptop", "mouse",
          "remote", "keyboard", "cell phone", "microwave", "oven", "toaster", "sink",
          "refrigerator", "book", "clock", "vase", "scissors", "teddy bear", "hair drier",
          "toothbrush"]


def letterbox(img, size=640):
    """等比缩放 + 灰边填充。注意：img 必须是 RGB（模型按 RGB 训练，喂 BGR 会掉精度）"""
    h, w = img.shape[:2]
    r = min(size / h, size / w)
    nh, nw = int(round(h * r)), int(round(w * r))
    resized = cv2.resize(img, (nw, nh), interpolation=cv2.INTER_LINEAR)
    canvas = np.full((size, size, 3), 114, dtype=np.uint8)
    top, left = (size - nh) // 2, (size - nw) // 2
    canvas[top:top + nh, left:left + nw] = resized
    return canvas, r, left, top


def sigmoid(x):
    return 1.0 / (1.0 + np.exp(-x))


def decode_yolov5(outputs, conf_thres=0.25, iou_thres=0.45):
    """YOLOv5 三尺度解码（RKNN 常见输出已做 sigmoid，若数值在 0~1 则不再 sigmoid）"""
    strides = [8, 16, 32]
    anchors = [[10, 13, 16, 30, 33, 23], [30, 61, 62, 45, 59, 119], [116, 90, 156, 198, 373, 326]]
    boxes, scores, classes = [], [], []
    for out, stride, anc in zip(outputs, strides, anchors):
        o = out[0] if out.ndim == 4 else out          # (255, H, W)
        _, gh, gw = o.shape
        o = o.reshape(3, 5 + len(COCO80), gh, gw)
        if o[:, 4].max() > 1.5 or o[:, 4].min() < -0.01:   # 未做 sigmoid 的原始输出
            o[:, 4:] = sigmoid(o[:, 4:])
        grid_x, grid_y = np.meshgrid(np.arange(gw), np.arange(gh))
        for a in range(3):
            bw = (sigmoid(o[a, 2]) * 2) ** 2 * anc[a * 2]
            bh = (sigmoid(o[a, 3]) * 2) ** 2 * anc[a * 2 + 1]
            bx = (sigmoid(o[a, 0]) * 2 - 0.5 + grid_x) * stride
            by = (sigmoid(o[a, 1]) * 2 - 0.5 + grid_y) * stride
            conf = o[a, 4]
            cls = o[a, 5:]
            m = conf > conf_thres
            if not np.any(m):
                continue
            cid = np.argmax(cls, axis=0)
            score = conf * np.max(cls, axis=0)
            mm = m & (score > conf_thres)
            if not np.any(mm):
                continue
            ys, xs = np.nonzero(mm)
            for y, x in zip(ys, xs):
                cx, cy, w_, h_ = bx[y, x], by[y, x], bw[y, x], bh[y, x]
                boxes.append([cx - w_ / 2, cy - h_ / 2, w_, h_])
                scores.append(float(score[y, x]))
                classes.append(int(cid[y, x]))
    if not boxes:
        return []
    idx = cv2.dnn.NMSBoxes(boxes, scores, conf_thres, iou_thres)
    return [(classes[i], scores[i], boxes[i]) for i in np.array(idx).flatten()]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--model", required=True)
    ap.add_argument("--images", required=True, help="图片目录或单张图片")
    ap.add_argument("--num", type=int, default=8, help="抽多少张做检出统计")
    ap.add_argument("--runs", type=int, default=20, help="延迟测量次数")
    ap.add_argument("--size", type=int, default=640)
    args = ap.parse_args()

    files = sorted(glob.glob(os.path.join(args.images, "*.jpg"))) if os.path.isdir(args.images) \
        else [args.images]
    if not files:
        print("没有可用图片"); return 2
    random.seed(1)
    files = random.sample(files, min(args.num, len(files)))

    print(f"模型: {args.model} ({os.path.getsize(args.model)/1048576:.1f} MB)")
    rknn = RKNNLite()
    if rknn.load_rknn(args.model) != 0:
        print("模型加载失败"); return 2
    if rknn.init_runtime() != 0:
        print("NPU 运行时初始化失败"); return 2
    print("NPU 运行时就绪 ✓")

    outs = rknn.inference(inputs=[letterbox(cv2.cvtColor(cv2.imread(files[0]), cv2.COLOR_BGR2RGB), args.size)[0][None, ...]])
    shapes = [tuple(o.shape) for o in outs]
    print(f"输出张量: {shapes}")
    yolov5_layout = len(outs) == 3 and outs[0].shape[1] == 255
    if not yolov5_layout:
        print("提示：输出不是 YOLOv5(255通道) 布局 → 若为 (1,84,N) 需改用 YOLOv8 解码")

    # ── 延迟测量 ──
    buf = cv2.cvtColor(cv2.imread(files[0]), cv2.COLOR_BGR2RGB)
    lb, _, _, _ = letterbox(buf, args.size)
    inp = lb[None, ...]
    for _ in range(3):
        rknn.inference(inputs=[inp])
    ts = []
    for _ in range(args.runs):
        t0 = time.perf_counter()
        rknn.inference(inputs=[inp])
        ts.append((time.perf_counter() - t0) * 1000)
    print(f"单帧推理: 均值 {statistics.mean(ts):.1f} ms  中位 {statistics.median(ts):.1f} ms  "
          f"最小 {min(ts):.1f} ms  → 约 {1000/statistics.mean(ts):.1f} fps")

    # ── 真实图片检出 ──
    print("\n真实图片检出（YOLOv5 解码 + NMS，conf>0.25）:")
    total = 0
    for f in files:
        img = cv2.cvtColor(cv2.imread(f), cv2.COLOR_BGR2RGB)
        lb, r, dx, dy = letterbox(img, args.size)
        t0 = time.perf_counter()
        outs = rknn.inference(inputs=[lb[None, ...]])
        dt = (time.perf_counter() - t0) * 1000
        dets = decode_yolov5(outs) if yolov5_layout else []
        named = [f"{COCO80[c] if c < len(COCO80) else c}:{s:.2f}" for c, s, _ in dets[:5]]
        total += len(dets)
        print(f"  {os.path.basename(f)}  推理 {dt:.0f} ms  检出 {len(dets)} 个  {' '.join(named)}")

    rknn.release()
    print(f"\n合计检出 {total} 个目标（{len(files)} 张图）")
    print("PASS: NPU 部署链路可用（模型加载 → 推理 → 解码 → 检出）")
    return 0


if __name__ == "__main__":
    sys.exit(main())

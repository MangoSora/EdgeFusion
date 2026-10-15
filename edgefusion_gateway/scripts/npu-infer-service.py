#!/usr/bin/env python3
"""板端 NPU 推理服务（轨道 A）：把 RK3576 的 NPU 变成与云端可互换的"分析后端"。

架构（进程隔离，推理崩了影响不到录像/预览主链路）：

    网关 /api/snapshot ──取帧──▶ 本服务(NPU 推理) ──POST /api/events──▶ 事件库/Web
                                        │
                                        └─ 支持模型：YOLOv5(255 通道) / YOLO11(84 通道, anchor-free)
                                          按输出形状自动选择解码器

用法（板子上，带 rknnlite 的 venv）：
    ~/venvs/rknn-lite2/bin/python scripts/npu-infer-service.py \
        --model <xxx.rknn> --fps 2 --classes person --conf 0.35 --cooldown 10

参数：
    --gateway     网关地址（默认 http://127.0.0.1:8080）
    --fps         每秒取帧推理次数（默认 2；NPU 能到 20+，这里只是按业务需要节流）
    --classes     关注的类别（逗号分隔，默认 person；"all" 表示全部）
    --conf        置信度阈值（默认 0.35）
    --cooldown    同类事件冷却秒数（默认 10，避免每帧一条事件）
    --duration    运行时长秒（0=一直跑，Ctrl+C 退出）
    --dry-run     只推理不提交事件（用于调参）
"""
import argparse
import base64
import json
import statistics
import sys
import time
import urllib.error
import urllib.request

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


def sigmoid(x):
    return 1.0 / (1.0 + np.exp(-x))


def letterbox(img, size=640):
    """等比缩放 + 灰边填充（img 必须是 RGB）"""
    h, w = img.shape[:2]
    r = min(size / h, size / w)
    nh, nw = int(round(h * r)), int(round(w * r))
    resized = cv2.resize(img, (nw, nh), interpolation=cv2.INTER_LINEAR)
    canvas = np.full((size, size, 3), 114, dtype=np.uint8)
    top, left = (size - nh) // 2, (size - nw) // 2
    canvas[top:top + nh, left:left + nw] = resized
    return canvas, r, left, top


def decode_yolov5(outputs, conf_thres, iou_thres):
    """YOLOv5：三尺度 (1,255,H,W)，带 objectness"""
    strides, anchors = [8, 16, 32], [[10, 13, 16, 30, 33, 23], [30, 61, 62, 45, 59, 119],
                                     [116, 90, 156, 198, 373, 326]]
    boxes, scores, classes = [], [], []
    for out, stride, anc in zip(outputs, strides, anchors):
        o = out[0] if out.ndim == 4 else out
        _, gh, gw = o.shape
        o = o.reshape(3, 5 + len(COCO80), gh, gw)
        if o[:, 4].max() > 1.5 or o[:, 4].min() < -0.01:
            o[:, 4:] = sigmoid(o[:, 4:])
        gx, gy = np.meshgrid(np.arange(gw), np.arange(gh))
        for a in range(3):
            bw = (sigmoid(o[a, 2]) * 2) ** 2 * anc[a * 2]
            bh = (sigmoid(o[a, 3]) * 2) ** 2 * anc[a * 2 + 1]
            bx = (sigmoid(o[a, 0]) * 2 - 0.5 + gx) * stride
            by = (sigmoid(o[a, 1]) * 2 - 0.5 + gy) * stride
            conf = o[a, 4]
            cls = o[a, 5:]
            cid = np.argmax(cls, axis=0)
            score = conf * np.max(cls, axis=0)
            mm = (conf > conf_thres) & (score > conf_thres)
            if not np.any(mm):
                continue
            ys, xs = np.nonzero(mm)
            for y, x in zip(ys, xs):
                boxes.append([bx[y, x] - bw[y, x] / 2, by[y, x] - bh[y, x] / 2, bw[y, x], bh[y, x]])
                scores.append(float(score[y, x]))
                classes.append(int(cid[y, x]))
    if not boxes:
        return []
    idx = cv2.dnn.NMSBoxes(boxes, scores, conf_thres, iou_thres)
    return [(classes[i], scores[i], boxes[i]) for i in np.array(idx).flatten()]


def decode_yolo11(outputs, conf_thres, iou_thres):
    """YOLO11/v8：(1,84,N) 无 objectness，前 4 维为 cx,cy,w,h（已是像素尺度），后 80 为类别分数"""
    o = outputs[0]
    o = o[0] if o.ndim == 3 else o                 # (84, N)
    if o.shape[0] < o.shape[1]:
        o = o.T                                    # (N, 84)
    boxes, scores, classes = [], [], []
    cls_scores = o[:, 4:]
    cid = np.argmax(cls_scores, axis=1)
    conf = cls_scores[np.arange(len(o)), cid]
    for i in np.nonzero(conf > conf_thres)[0]:
        cx, cy, w, h = o[i, 0], o[i, 1], o[i, 2], o[i, 3]
        boxes.append([float(cx - w / 2), float(cy - h / 2), float(w), float(h)])
        scores.append(float(conf[i]))
        classes.append(int(cid[i]))
    if not boxes:
        return []
    idx = cv2.dnn.NMSBoxes(boxes, scores, conf_thres, iou_thres)
    return [(classes[i], scores[i], boxes[i]) for i in np.array(idx).flatten()]


def fetch(url, timeout=5):
    with urllib.request.urlopen(url, timeout=timeout) as r:
        return r.read()


def post_event(gateway, payload, timeout=10):
    data = json.dumps(payload).encode()
    req = urllib.request.Request(gateway + "/api/events", data=data,
                                 headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return json.loads(r.read())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--model", required=True)
    ap.add_argument("--gateway", default="http://127.0.0.1:8080")
    ap.add_argument("--fps", type=float, default=2.0)
    ap.add_argument("--classes", default="person")
    ap.add_argument("--conf", type=float, default=0.35)
    ap.add_argument("--iou", type=float, default=0.45)
    ap.add_argument("--cooldown", type=float, default=10.0)
    ap.add_argument("--size", type=int, default=640)
    ap.add_argument("--duration", type=float, default=0)
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()

    want = None if args.classes.lower() == "all" else {c.strip() for c in args.classes.split(",")}

    rknn = RKNNLite()
    if rknn.load_rknn(args.model) != 0:
        print("模型加载失败"); return 2
    if rknn.init_runtime() != 0:
        print("NPU 初始化失败"); return 2

    # 先跑一次确定解码方式
    probe = None
    for _ in range(20):
        try:
            probe = fetch(args.gateway + "/api/snapshot")
            break
        except Exception:
            time.sleep(0.5)
    if not probe:
        print(f"取不到画面：{args.gateway}/api/snapshot"); return 2
    img0 = cv2.cvtColor(cv2.imdecode(np.frombuffer(probe, np.uint8), cv2.IMREAD_COLOR),
                        cv2.COLOR_BGR2RGB)
    outs = rknn.inference(inputs=[letterbox(img0, args.size)[0][None, ...]])
    shapes = [tuple(o.shape) for o in outs]
    if len(outs) == 3 and outs[0].shape[1] == 255:
        decoder, kind = decode_yolov5, "YOLOv5(255ch, anchor-based)"
    else:
        decoder, kind = decode_yolo11, "YOLO11/YOLOv8(anchor-free)"
    print(f"[npu] 模型 {args.model}")
    print(f"[npu] 输出 {shapes} → 解码器 {kind}")
    print(f"[npu] 关注类别 {sorted(want) if want else 'all'} | conf>{args.conf} | 冷却 {args.cooldown}s | 目标 {args.fps} fps")

    last_fire = {}
    t_end = time.time() + args.duration if args.duration > 0 else None
    lat, n_frame, n_event, n_det = [], 0, 0, 0
    t_stat = time.time()

    try:
        while True:
            if t_end and time.time() >= t_end:
                break
            t0 = time.time()
            try:
                jpg = fetch(args.gateway + "/api/snapshot")
            except Exception as e:                       # noqa: BLE001
                print(f"[npu] 取帧失败: {e}"); time.sleep(1); continue
            img = cv2.cvtColor(cv2.imdecode(np.frombuffer(jpg, np.uint8), cv2.IMREAD_COLOR),
                               cv2.COLOR_BGR2RGB)
            lb, _, _, _ = letterbox(img, args.size)
            ti = time.time()
            outs = rknn.inference(inputs=[lb[None, ...]])
            lat.append((time.time() - ti) * 1000)
            n_frame += 1

            dets = decoder(outs, args.conf, args.iou)
            hits = []
            for cid, score, box in dets:
                name = COCO80[cid] if cid < len(COCO80) else f"cls{cid}"
                if want is None or name in want:
                    hits.append((name, score, box))
            n_det += len(hits)

            now = time.time()
            fired = []
            for name, score, _ in hits:
                if now - last_fire.get(name, 0) >= args.cooldown:
                    last_fire[name] = now
                    fired.append((name, score))
            if fired and not args.dry_run:
                best_name, best_conf = max(fired, key=lambda x: x[1])
                desc = "NPU 检测: " + ", ".join(f"{n}({s:.2f})" for n, s in fired[:4])
                try:
                    res = post_event(args.gateway, {
                        "event_type": "npu_" + best_name,
                        "confidence": round(float(best_conf), 3),
                        "description": desc,
                        "suggested_action": "record",
                        "jpeg_base64": base64.b64encode(jpg).decode(),
                    })
                    n_event += 1
                    print(f"[npu] 事件入库 id={res.get('event_id')} {desc}")
                except Exception as e:                   # noqa: BLE001
                    print(f"[npu] 事件提交失败: {e}")

            if time.time() - t_stat >= 10:
                t_stat = time.time()
                print(f"[npu] 统计: 推理 {n_frame} 帧, 平均 {statistics.mean(lat):.1f} ms, "
                      f"命中 {n_det} 个目标, 提交事件 {n_event} 条")
                lat.clear()

            dt = time.time() - t0
            time.sleep(max(0.0, 1.0 / args.fps - dt))
    except KeyboardInterrupt:
        print("\n[npu] 收到 Ctrl+C，退出")
    finally:
        rknn.release()

    print(f"[npu] 结束: 共推理 {n_frame} 帧（平均 {statistics.mean([l for l in lat] or [0]):.1f} ms），"
          f"命中 {n_det} 个目标，提交事件 {n_event} 条")
    return 0


if __name__ == "__main__":
    sys.exit(main())

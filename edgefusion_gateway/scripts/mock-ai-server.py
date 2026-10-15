#!/usr/bin/env python3
"""模拟 OpenAI 兼容的云端 AI 服务（测试用，不需要真实 API Key）。

用途：在没有真实密钥的情况下验证网关的「云端 AI」链路是否通：
    端侧快照/本地帧 → cloud_ai → HTTP POST → 本服务 → 返回结构化事件 JSON → 事件入库。

用法（板子上）：
    python3 scripts/mock-ai-server.py --port 9099
然后在网关配置里指向它：
    anthropic_base_url = http://127.0.0.1:9099/v1     # 代码会拼 /chat/completions
    anthropic_auth_token = sk-mock
    anthropic_model = mock-model
    ai_enable = 1
    ai_source = device        # 分析端侧上报的快照（local/device/both）

它会打印每条请求（路径、字节数、是否含图片、Authorization 前缀）并追加到
/tmp/mock-ai-requests.log，便于核对"到底有没有把画面送上去"。
"""
import argparse
import json
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

LOG_PATH = "/tmp/mock-ai-requests.log"


class Handler(BaseHTTPRequestHandler):
    reply = {
        "event_type": "person_detected",
        "confidence": 0.88,
        "description": "画面中检测到人员（mock AI 返回）",
        "suggested_action": "record",
    }

    def do_POST(self):                                   # noqa: N802
        n = int(self.headers.get("Content-Length", "0"))
        raw = self.rfile.read(n).decode("utf-8", "ignore")
        try:
            body = json.loads(raw)
        except Exception:                                # noqa: BLE001
            body = {}

        # 判断请求里是否真的带了图片（OpenAI 多模态格式：messages[].content[].image_url）
        has_image = "image_url" in raw or "base64" in raw
        msgs = body.get("messages") or []
        text_len = 0
        try:
            for m in msgs:
                c = m.get("content")
                if isinstance(c, list):
                    for blk in c:
                        if blk.get("type") == "text":
                            text_len += len(blk.get("text", ""))
        except Exception:                                # noqa: BLE001
            pass

        info = {
            "t": time.strftime("%H:%M:%S"),
            "path": self.path,
            "bytes": len(raw),
            "has_image": has_image,
            "model": body.get("model"),
            "prompt_chars": text_len,
            "auth_prefix": self.headers.get("Authorization", "")[:16],
        }
        print("[mock-ai] " + json.dumps(info, ensure_ascii=False), flush=True)
        try:
            with open(LOG_PATH, "a", encoding="utf-8") as f:
                f.write(json.dumps(info, ensure_ascii=False) + "\n")
        except OSError:
            pass

        content = json.dumps(self.reply, ensure_ascii=False)
        resp = json.dumps({"choices": [{"message": {"role": "assistant", "content": content}}]},
                          ensure_ascii=False).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(resp)))
        self.end_headers()
        self.wfile.write(resp)

    def log_message(self, *args):                        # 静音默认访问日志
        pass


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=9099)
    ap.add_argument("--event-type", default="person_detected",
                    help="返回给网关的事件类型（none = 模拟无异常）")
    ap.add_argument("--confidence", type=float, default=0.88)
    ap.add_argument("--description", default="画面中检测到人员（mock AI 返回）")
    args = ap.parse_args()

    if args.event_type == "none":
        Handler.reply = {"event_type": "none", "confidence": 0.0,
                         "description": "画面正常（mock）", "suggested_action": "ignore"}
    else:
        Handler.reply = {"event_type": args.event_type, "confidence": args.confidence,
                         "description": args.description, "suggested_action": "record"}

    srv = ThreadingHTTPServer(("127.0.0.1", args.port), Handler)
    print(f"[mock-ai] 监听 127.0.0.1:{args.port}，返回事件 {Handler.reply}", flush=True)
    print(f"[mock-ai] 请求日志: {LOG_PATH}", flush=True)
    srv.serve_forever()


if __name__ == "__main__":
    main()

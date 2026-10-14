#!/usr/bin/env python3
"""离线校验 slow-rtsp-client 的模拟网关（测试辅助，不是项目运行代码）。

它把板端两个行为搬到本机，用来在不接触开发板的情况下验证复现工具本身是否可靠：

  1. RTSP 服务端：回应 OPTIONS/DESCRIBE/SETUP/PLAY/TEARDOWN，PLAY 后持续发
     interleaved 帧；并记录客户端是否带上 Session 头、是否发出 TEARDOWN
     （后者能证明 `$` 帧跳过与响应解析没写坏）。
  2. HTTP 服务端：`/api/status` 在有“停读客户端”存在时额外 sleep 2 秒，
     模拟 rtsp_server 持锁阻塞导致状态接口变慢。

用法（任意目录，只要脚本仍在仓库里）：

    python3 tests/mock_gateway.py

判定：客户端应打印 REPRODUCED，且末尾出现 `MOCK CHECK: PASS`（退出码 0）。
"""
import importlib.util
import json
import pathlib
import re
import socket
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

HTTP_PORT = 18099
RTSP_PORT = 18554
SESSION_ID = b"12345678"
STALL_WINDOW = 12.0     # PLAY 之后多久内状态接口变慢
STATUS_DELAY = 2.0

state = {"stalled_until": 0.0, "teardown": False, "play_session": None, "frames_sent": 0}


def load_client():
    """按路径加载 scripts/slow-rtsp-client.py（文件名带连字符，不能直接 import）。"""
    path = pathlib.Path(__file__).resolve().parent.parent / "scripts" / "slow-rtsp-client.py"
    spec = importlib.util.spec_from_file_location("slow_rtsp_client", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class StatusHandler(BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path != "/api/status":
            self.send_error(404)
            return
        if time.time() < state["stalled_until"]:
            time.sleep(STATUS_DELAY)          # 模拟发送线程持锁导致状态接口变慢
        body = json.dumps({
            "stream": {
                "connected": True, "width": 640, "height": 480, "fps": 30,
                "recording": True, "cur_segment": "rec_mock_0.mp4", "bitrate_kbps": 1500,
                "source_type": "v4l2", "frames": 316, "reconnects": 0,
                "preview_queue": {"depth": 0, "high": 2, "dropped": 0, "processed": 42},
                "preview_raw_jpeg": True,
            }
        }).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *args):
        pass


def rtsp_serve():
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", RTSP_PORT))
    srv.listen(1)
    conn, _ = srv.accept()
    buf = b""

    def read_request():
        nonlocal buf
        while b"\r\n\r\n" not in buf:
            data = conn.recv(4096)
            if not data:
                return None
            buf += data
        head, _, buf = buf.partition(b"\r\n\r\n")
        return head.decode("utf-8", "replace")

    def pump():
        frame = b"$" + bytes([0, 0]) + (1200).to_bytes(2, "little") + bytes(1200)
        while True:
            try:
                conn.sendall(frame)
                state["frames_sent"] += 1
                time.sleep(0.002)
            except OSError:
                return

    try:
        while True:
            head = read_request()
            if head is None:
                break
            method = head.split()[0]
            cseq_match = re.search(r"(?im)^cseq:\s*(\d+)", head)
            cseq = cseq_match.group(1) if cseq_match else "1"
            session_match = re.search(r"(?im)^session:\s*(\S+)", head)
            print(f"  mock RTSP <- {method}" +
                  (f" (Session: {session_match.group(1)})" if session_match else ""))
            if method == "OPTIONS":
                conn.sendall(f"RTSP/1.0 200 OK\r\nCSeq: {cseq}\r\n"
                             f"Public: OPTIONS, DESCRIBE, SETUP, PLAY, TEARDOWN\r\n\r\n".encode())
            elif method == "DESCRIBE":
                sdp = b"v=0\r\no=- 0 0 IN IP4 127.0.0.1\r\ns=mock\r\n"
                sdp += b"m=video 0 RTP/AVP 96\r\na=control:track0\r\n"
                conn.sendall(f"RTSP/1.0 200 OK\r\nCSeq: {cseq}\r\n"
                             f"Content-Type: application/sdp\r\n"
                             f"Content-Length: {len(sdp)}\r\n\r\n".encode() + sdp)
            elif method == "SETUP":
                conn.sendall(f"RTSP/1.0 200 OK\r\nCSeq: {cseq}\r\n"
                             f"Session: {SESSION_ID.decode()}\r\n"
                             f"Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n\r\n".encode())
            elif method == "PLAY":
                state["play_session"] = session_match.group(1) if session_match else None
                state["stalled_until"] = time.time() + STALL_WINDOW
                conn.sendall(f"RTSP/1.0 200 OK\r\nCSeq: {cseq}\r\n"
                             f"Session: {SESSION_ID.decode()}\r\n\r\n".encode())
                threading.Thread(target=pump, daemon=True).start()
            elif method == "TEARDOWN":
                state["teardown"] = True
                conn.sendall(f"RTSP/1.0 200 OK\r\nCSeq: {cseq}\r\n\r\n".encode())
                break
    except OSError:
        pass
    finally:
        try:
            conn.close()
        except OSError:
            pass


def main():
    client_mod = load_client()
    http = ThreadingHTTPServer(("127.0.0.1", HTTP_PORT), StatusHandler)
    threading.Thread(target=http.serve_forever, daemon=True).start()
    threading.Thread(target=rtsp_serve, daemon=True).start()
    time.sleep(0.3)

    print("--- 运行 slow-rtsp-client（模拟网关：RTSP 18554 / status 18099）---")
    sys.argv = ["slow-rtsp-client.py", "127.0.0.1", str(RTSP_PORT), "4",
                f"http://127.0.0.1:{HTTP_PORT}/api/status"]
    rc = client_mod.main()
    http.shutdown()

    print("--- 模拟网关观察 ---")
    print(f"PLAY 是否带 Session : {state['play_session']}")
    print(f"是否收到 TEARDOWN   : {state['teardown']}")
    print(f"发出的 interleaved 帧: {state['frames_sent']}")
    ok = (rc == 0 and state["play_session"] == SESSION_ID.decode()
          and state["teardown"] and state["frames_sent"] > 0)
    print("MOCK CHECK:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""慢 RTSP 客户端复现工具：只观察，不修改服务端状态。

背景（代码位置见 docs/RK3576_PORT.md 的“慢客户端隔离”一节）：
  rtsp_server_on_packet() 在持有 g_mtx 的情况下逐个客户端阻塞发送
  （rtsp_server.c:300-322，send_all() 为阻塞写，客户端套接字 SO_SNDTIMEO=2s）。
  因此“连上但不读数据”的客户端会让发送方卡住，连带阻塞
  rtsp_server_client_count()（/api/status 会调用）和新客户端接入。

本脚本做四件事：
  1. 建立 RTSP/TCP interleaved 会话（OPTIONS → DESCRIBE → SETUP → PLAY）；
  2. 在一段时间内完全不读取套接字，让服务端发送缓冲区堆积；
  3. 期间用 HTTP 探测 /api/status 的响应耗时（对比基线）；
  4. 恢复读取并统计积压字节数，最后打印 /api/status 的预览队列指标。

用法（在开发板上执行）：
    python3 scripts/slow-rtsp-client.py [host] [rtsp_port] [stall_seconds] [status_url]
默认：127.0.0.1 8554 15 http://127.0.0.1:8080/api/status
前提：网关配置里 rtsp_server_enable = 1
"""
import json
import re
import socket
import sys
import time
import urllib.request


class RtspClient:
    def __init__(self, host, port, timeout=5.0):
        self.sock = socket.create_connection((host, port), timeout=timeout)
        self.buf = b""
        self.cseq = 0
        self.session = None

    def _recv_some(self, timeout):
        self.sock.settimeout(timeout)
        data = self.sock.recv(65536)
        if not data:
            raise ConnectionError("server closed the connection")
        self.buf += data

    def _take_response(self):
        """从缓冲区取出一条完整响应；顺带跳过 interleaved RTP 帧。"""
        while True:
            if self.buf.startswith(b"$"):
                if len(self.buf) < 4:
                    return None
                length = self.buf[2] | (self.buf[3] << 8)
                if len(self.buf) < 4 + length:
                    return None
                self.buf = self.buf[4 + length:]
                continue
            end = self.buf.find(b"\r\n\r\n")
            if end < 0:
                return None
            head = self.buf[:end].decode("utf-8", "replace")
            rest = self.buf[end + 4:]
            match = re.search(r"(?im)^content-length:\s*(\d+)\s*$", head)
            length = int(match.group(1)) if match else 0
            if len(rest) < length:
                return None
            self.buf = rest[length:]
            return head, rest[:length]

    def request(self, method, url, headers=(), timeout=5.0):
        self.cseq += 1
        lines = [f"{method} {url} RTSP/1.0", f"CSeq: {self.cseq}",
                 "User-Agent: slow-client-repro"]
        if self.session:
            lines.append(f"Session: {self.session}")
        lines.extend(headers)
        self.sock.sendall(("\r\n".join(lines) + "\r\n\r\n").encode())

        deadline = time.time() + timeout
        while True:
            got = self._take_response()
            if got:
                head, body = got
                match = re.search(r"(?im)^session:\s*([0-9A-Fa-f]+)", head)
                if match:
                    self.session = match.group(1)
                return head, body
            if time.time() > deadline:
                raise TimeoutError(f"no response to {method}")
            self._recv_some(max(0.1, deadline - time.time()))

    def stall(self, seconds):
        """完全不读套接字，让服务端发送缓冲区堆满。"""
        time.sleep(seconds)

    def drain(self, seconds):
        """恢复读取，返回读到的字节数。"""
        total = 0
        end = time.time() + seconds
        while time.time() < end:
            try:
                self.sock.settimeout(0.2)
                data = self.sock.recv(262144)
                if not data:
                    break
                total += len(data)
            except socket.timeout:
                continue
            except OSError:
                break
        return total

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass


def status(url, timeout=10.0):
    """取 /api/status，返回 (耗时秒, JSON 或 None)。"""
    started = time.time()
    try:
        with urllib.request.urlopen(url, timeout=timeout) as resp:
            data = resp.read()
        return time.time() - started, json.loads(data.decode())
    except Exception:
        return time.time() - started, None


def sample_latency(url, rounds=3):
    out = []
    for _ in range(rounds):
        elapsed, payload = status(url)
        out.append(elapsed)
        time.sleep(0.2)
    return out


def fmt(values):
    return ", ".join(f"{v:.3f}" for v in values)


def main():
    host = sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1"
    port = int(sys.argv[2]) if len(sys.argv) > 2 else 8554
    stall_s = float(sys.argv[3]) if len(sys.argv) > 3 else 15.0
    status_url = sys.argv[4] if len(sys.argv) > 4 else "http://127.0.0.1:8080/api/status"

    base = sample_latency(status_url)
    _, before = status(status_url)
    print(f"baseline /api/status latency : {fmt(base)} s")
    if before:
        print(f"preview_queue before         : {before['stream'].get('preview_queue')}")
    else:
        print("preview_queue before         : (取不到，检查 Web 端口是否可达)")

    client = RtspClient(host, port)
    url = f"rtsp://{host}:{port}"
    try:
        head, _ = client.request("OPTIONS", url)
        print(f"OPTIONS  : {head.splitlines()[0]}")
        head, sdp = client.request("DESCRIBE", url, ["Accept: application/sdp"])
        print(f"DESCRIBE : {head.splitlines()[0]} ({len(sdp)} bytes SDP)")
        head, _ = client.request(
            "SETUP", f"{url}/track0",
            ["Transport: RTP/AVP/TCP;unicast;interleaved=0-1"])
        print(f"SETUP    : {head.splitlines()[0]} session={client.session}")
        head, _ = client.request("PLAY", url)
        print(f"PLAY     : {head.splitlines()[0]}")

        print(f"stalling {stall_s:g} s without reading ...")
        client.stall(stall_s)

        during = sample_latency(status_url)
        print(f"during-stall latency         : {fmt(during)} s")

        drained = client.drain(2.0)
        print(f"bytes readable after stall   : {drained}")

        after_latency = sample_latency(status_url)
        _, after = status(status_url)
        print(f"after latency                : {fmt(after_latency)} s")
        if after:
            print(f"preview_queue after          : {after['stream'].get('preview_queue')}")

        try:
            client.request("TEARDOWN", url)
        except Exception:
            pass
    finally:
        client.close()

    base_mean = sum(base) / len(base)
    stall_mean = sum(during) / len(during)
    print()
    if stall_mean > max(0.05, base_mean * 5):
        print(f"REPRODUCED: /api/status 从 {base_mean:.3f}s 恶化到 {stall_mean:.3f}s "
              f"（{stall_mean / max(base_mean, 1e-6):.0f}x）")
    else:
        print(f"NOT REPRODUCED: 基线 {base_mean:.3f}s，卡住期间 {stall_mean:.3f}s；"
              f"可加大 stall 秒数或确认 rtsp_server_enable=1")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as exc:  # 复现工具：失败也要给出可读原因
        print(f"ERROR: {exc}", file=sys.stderr)
        sys.exit(1)

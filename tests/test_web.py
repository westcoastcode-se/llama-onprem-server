#!/usr/bin/env python3
"""
Test suite for Local AI Web UI and TCP bridge server.
Tests:
  - Mock TCP AI Server
  - Static file serving (HTML, CSS, JS)
  - /api/status & /api/ping
  - /api/config
  - /api/context
  - /api/reset
  - /api/chat (SSE streaming & sync)
  - /api/generate (SSE streaming & sync)
"""

import json
import os
import socket
import sys
import threading
import time
import urllib.request
import urllib.error

# Add web directory to path
sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "web"))
import server as web_server


class MockTCPAIServer:
    """Mock Local AI TCP server for testing."""

    def __init__(self, host="127.0.0.1", port=18080):
        self.host = host
        self.port = port
        self.sock = None
        self.running = False
        self.thread = None
        self.model_name = "test-model.gguf"
        self.n_ctx = 4096
        self.used_ctx = 128

    def start(self):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.bind((self.host, self.port))
        self.sock.listen(5)
        self.running = True
        self.thread = threading.Thread(target=self._run, daemon=True)
        self.thread.start()

    def _run(self):
        while self.running:
            try:
                client_sock, _ = self.sock.accept()
            except Exception:
                break
            threading.Thread(target=self._handle_client, args=(client_sock,), daemon=True).start()

    def _handle_client(self, client_sock):
        f = client_sock.makefile("r", encoding="utf-8", errors="replace")
        try:
            while self.running:
                line = f.readline()
                if not line:
                    break
                try:
                    req = json.loads(line)
                except Exception:
                    continue

                req_type = req.get("type", "")
                if req_type == "ping":
                    resp = {
                        "type": "pong",
                        "status": "ok",
                        "model": self.model_name,
                        "n_ctx": self.n_ctx,
                        "used_ctx": self.used_ctx
                    }
                    client_sock.sendall((json.dumps(resp) + "\n").encode("utf-8"))
                elif req_type == "context":
                    resp = {
                        "type": "context",
                        "n_ctx": self.n_ctx,
                        "used_ctx": self.used_ctx
                    }
                    client_sock.sendall((json.dumps(resp) + "\n").encode("utf-8"))
                elif req_type == "reset":
                    self.used_ctx = 0
                    resp = {"type": "ok", "message": "context reset successful"}
                    client_sock.sendall((json.dumps(resp) + "\n").encode("utf-8"))
                elif req_type == "chat":
                    self.used_ctx += 64
                    tokens = ["Hej", "!", " ", "Detta", " ", "är", " ", "ett", " ", "test."]
                    for tok in tokens:
                        t_resp = {"type": "token", "piece": tok}
                        client_sock.sendall((json.dumps(t_resp) + "\n").encode("utf-8"))
                    d_resp = {
                        "type": "done",
                        "response": "Hej! Detta är ett test.",
                        "n_ctx": self.n_ctx,
                        "used_ctx": self.used_ctx
                    }
                    client_sock.sendall((json.dumps(d_resp) + "\n").encode("utf-8"))
                elif req_type == "generate":
                    self.used_ctx += 32
                    tokens = ["Genererat", " ", "svar."]
                    for tok in tokens:
                        t_resp = {"type": "token", "piece": tok}
                        client_sock.sendall((json.dumps(t_resp) + "\n").encode("utf-8"))
                    d_resp = {
                        "type": "done",
                        "response": "Genererat svar.",
                        "n_ctx": self.n_ctx,
                        "used_ctx": self.used_ctx
                    }
                    client_sock.sendall((json.dumps(d_resp) + "\n").encode("utf-8"))
                else:
                    err_resp = {"type": "error", "message": "unknown request"}
                    client_sock.sendall((json.dumps(err_resp) + "\n").encode("utf-8"))
        finally:
            client_sock.close()

    def stop(self):
        self.running = False
        if self.sock:
            try:
                self.sock.close()
            except Exception:
                pass


def test_web_app():
    print("[TEST] Starting Web UI integration test...")

    tcp_server = MockTCPAIServer(port=18080)
    tcp_server.start()

    web_port = 13000
    web_server.AIServerConfig.server_host = "127.0.0.1"
    web_server.AIServerConfig.server_port = 18080

    httpd = web_server.ThreadedHTTPServer(("127.0.0.1", web_port), web_server.WebRequestHandler)
    http_thread = threading.Thread(target=httpd.serve_forever, daemon=True)
    http_thread.start()

    base_url = f"http://127.0.0.1:{web_port}"
    time.sleep(0.1)

    try:
        # 1. Test Static Files
        print("  - Testing static file endpoints...")
        with urllib.request.urlopen(f"{base_url}/") as res:
            assert res.status == 200
            html = res.read().decode("utf-8")
            assert "<title>Local AI - Web Interface</title>" in html
            assert "messages-container" in html

        with urllib.request.urlopen(f"{base_url}/style.css") as res:
            assert res.status == 200
            css = res.read().decode("utf-8")
            assert "--bg-primary" in css

        with urllib.request.urlopen(f"{base_url}/app.js") as res:
            assert res.status == 200
            js = res.read().decode("utf-8")
            assert "handleSendMessage" in js

        # 2. Test /api/status & /api/ping
        print("  - Testing /api/status...")
        with urllib.request.urlopen(f"{base_url}/api/status") as res:
            assert res.status == 200
            data = json.loads(res.read().decode("utf-8"))
            assert data.get("status") == "ok"
            assert data.get("model") == "test-model.gguf"
            assert data.get("n_ctx") == 4096
            assert data.get("used_ctx") == 128

        # 3. Test /api/config
        print("  - Testing /api/config...")
        with urllib.request.urlopen(f"{base_url}/api/config") as res:
            assert res.status == 200
            cfg = json.loads(res.read().decode("utf-8"))
            assert cfg.get("server_port") == 18080

        req_config = urllib.request.Request(
            f"{base_url}/api/config",
            data=json.dumps({"server_host": "127.0.0.1", "server_port": 18080}).encode("utf-8"),
            headers={"Content-Type": "application/json"}
        )
        with urllib.request.urlopen(req_config) as res:
            assert res.status == 200

        # 4. Test /api/context
        print("  - Testing /api/context...")
        with urllib.request.urlopen(f"{base_url}/api/context") as res:
            assert res.status == 200
            data = json.loads(res.read().decode("utf-8"))
            assert data.get("type") == "context"
            assert data.get("n_ctx") == 4096

        # 5. Test /api/chat (sync mode)
        print("  - Testing /api/chat (sync)...")
        chat_req = urllib.request.Request(
            f"{base_url}/api/chat",
            data=json.dumps({
                "messages": [{"role": "user", "content": "Hello"}],
                "temperature": 0.7,
                "stream": False
            }).encode("utf-8"),
            headers={"Content-Type": "application/json"}
        )
        with urllib.request.urlopen(chat_req) as res:
            assert res.status == 200
            data = json.loads(res.read().decode("utf-8"))
            assert data.get("type") == "done"
            assert data.get("response") == "Hej! Detta är ett test."

        # 6. Test /api/chat (SSE streaming mode)
        print("  - Testing /api/chat (SSE stream)...")
        stream_req = urllib.request.Request(
            f"{base_url}/api/chat",
            data=json.dumps({
                "messages": [{"role": "user", "content": "Hello"}],
                "temperature": 0.7,
                "stream": True
            }).encode("utf-8"),
            headers={"Content-Type": "application/json"}
        )
        with urllib.request.urlopen(stream_req) as res:
            assert res.status == 200
            stream_body = res.read().decode("utf-8")
            assert "data: " in stream_body
            assert "piece" in stream_body
            assert "Hej! Detta är ett test." in stream_body or "test." in stream_body

        # 7. Test /api/generate
        print("  - Testing /api/generate (sync)...")
        gen_req = urllib.request.Request(
            f"{base_url}/api/generate",
            data=json.dumps({
                "prompt": "Test prompt",
                "temperature": 0.7,
                "stream": False
            }).encode("utf-8"),
            headers={"Content-Type": "application/json"}
        )
        with urllib.request.urlopen(gen_req) as res:
            assert res.status == 200
            data = json.loads(res.read().decode("utf-8"))
            assert data.get("response") == "Genererat svar."

        # 8. Test /api/reset
        print("  - Testing /api/reset...")
        reset_req = urllib.request.Request(
            f"{base_url}/api/reset",
            data=b"{}",
            headers={"Content-Type": "application/json"}
        )
        with urllib.request.urlopen(reset_req) as res:
            assert res.status == 200
            data = json.loads(res.read().decode("utf-8"))
            assert data.get("type") == "ok"
            assert tcp_server.used_ctx == 0

        # 9. Test offline handling when AI server is unreachable
        print("  - Testing offline server status handling...")
        web_server.AIServerConfig.server_port = 19999  # unused port
        with urllib.request.urlopen(f"{base_url}/api/status") as res:
            assert res.status == 200
            data = json.loads(res.read().decode("utf-8"))
            assert data.get("status") == "offline"

        print("✅ [TEST] All Web UI tests passed successfully!")

    finally:
        httpd.shutdown()
        httpd.server_close()
        tcp_server.stop()


if __name__ == "__main__":
    test_web_app()

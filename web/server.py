#!/usr/bin/env python3
"""
Standalone Web Server & Bridge for Local AI Server.
Connects the web frontend (HTML/JS/CSS) to the Local AI TCP server over raw TCP sockets.
No external dependencies required (uses standard library).
"""

import argparse
import http.server
import json
import os
import socket
import sys
import threading
import time
import urllib.parse

DEFAULT_WEB_PORT = 3000
DEFAULT_WEB_HOST = "0.0.0.0"
DEFAULT_AI_SERVER_HOST = "127.0.0.1"
DEFAULT_AI_SERVER_PORT = 8080

STATIC_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "static")


class LocalAIServerClient:
    """Client for communicating with the C++ Local AI TCP server over newline-delimited JSON."""

    def __init__(self, host=DEFAULT_AI_SERVER_HOST, port=DEFAULT_AI_SERVER_PORT, timeout=30.0):
        self.host = host
        self.port = port
        self.timeout = timeout

    def _connect(self):
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.settimeout(self.timeout)
        sock.connect((self.host, self.port))
        return sock

    def send_request(self, req_data):
        """Send a single request and return the single response."""
        sock = self._connect()
        try:
            req_line = json.dumps(req_data) + "\n"
            sock.sendall(req_line.encode("utf-8"))

            f = sock.makefile("r", encoding="utf-8", errors="replace")
            line = f.readline()
            if not line:
                raise ConnectionError("Empty response from server")
            return json.loads(line)
        finally:
            sock.close()

    def stream_chat(self, messages, temperature=0.7):
        """Stream chat tokens from the TCP server yielding chunks/events."""
        sock = self._connect()
        try:
            req = {
                "type": "chat",
                "messages": messages,
                "temperature": temperature
            }
            req_line = json.dumps(req) + "\n"
            sock.sendall(req_line.encode("utf-8"))

            f = sock.makefile("r", encoding="utf-8", errors="replace")
            while True:
                line = f.readline()
                if not line:
                    break
                try:
                    data = json.loads(line)
                except json.JSONDecodeError:
                    continue
                yield data
                if data.get("type") == "done" or data.get("type") == "error":
                    break
        finally:
            sock.close()

    def stream_generate(self, prompt, temperature=0.7):
        """Stream generation tokens from the TCP server."""
        sock = self._connect()
        try:
            req = {
                "type": "generate",
                "prompt": prompt,
                "temperature": temperature
            }
            req_line = json.dumps(req) + "\n"
            sock.sendall(req_line.encode("utf-8"))

            f = sock.makefile("r", encoding="utf-8", errors="replace")
            while True:
                line = f.readline()
                if not line:
                    break
                try:
                    data = json.loads(line)
                except json.JSONDecodeError:
                    continue
                yield data
                if data.get("type") == "done" or data.get("type") == "error":
                    break
        finally:
            sock.close()


class AIServerConfig:
    server_host = DEFAULT_AI_SERVER_HOST
    server_port = DEFAULT_AI_SERVER_PORT


class WebRequestHandler(http.server.SimpleHTTPRequestHandler):
    """Handles static files and API proxy requests to the AI server."""

    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=STATIC_DIR, **kwargs)

    def log_message(self, format, *args):
        # Clean custom logging
        sys.stdout.write(f"[web] {self.address_string()} - {format % args}\n")
        sys.stdout.flush()

    def _send_json_response(self, data, status=200):
        body = json.dumps(data).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Cache-Control", "no-cache, no-store, must-revalidate")
        self.end_headers()
        self.wfile.write(body)

    def _send_error_json(self, message, status=500):
        self._send_json_response({"type": "error", "message": str(message)}, status=status)

    def do_OPTIONS(self):
        self.send_response(204)
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS")
        self.send_header("Access-Control-Allow-Headers", "Content-Type")
        self.end_headers()

    def do_GET(self):
        parsed = urllib.parse.urlparse(self.path)
        path = parsed.path

        if path == "/api/status" or path == "/api/ping":
            self._handle_ping()
        elif path == "/api/context":
            self._handle_context()
        elif path == "/api/config":
            self._send_json_response({
                "server_host": AIServerConfig.server_host,
                "server_port": AIServerConfig.server_port
            })
        else:
            # Fallback for SPA routing: serve index.html for root or missing pages
            if path == "/":
                self.path = "/index.html"
            super().do_GET()

    def do_POST(self):
        parsed = urllib.parse.urlparse(self.path)
        path = parsed.path

        content_length = int(self.headers.get("Content-Length", 0))
        post_data = self.rfile.read(content_length) if content_length > 0 else b"{}"

        try:
            req_json = json.loads(post_data.decode("utf-8")) if post_data else {}
        except json.JSONDecodeError:
            self._send_error_json("Invalid JSON payload in request", 400)
            return

        if path == "/api/config":
            new_host = req_json.get("server_host")
            new_port = req_json.get("server_port")
            if new_host:
                AIServerConfig.server_host = str(new_host).strip()
            if new_port:
                try:
                    AIServerConfig.server_port = int(new_port)
                except ValueError:
                    pass
            self._send_json_response({
                "type": "ok",
                "server_host": AIServerConfig.server_host,
                "server_port": AIServerConfig.server_port
            })
        elif path == "/api/reset":
            self._handle_reset()
        elif path == "/api/chat":
            self._handle_chat(req_json)
        elif path == "/api/generate":
            self._handle_generate(req_json)
        else:
            self._send_error_json(f"Unknown endpoint: {path}", 404)

    def _get_client(self):
        return LocalAIServerClient(
            host=AIServerConfig.server_host,
            port=AIServerConfig.server_port
        )

    def _handle_ping(self):
        client = self._get_client()
        try:
            resp = client.send_request({"type": "ping"})
            resp["target_host"] = AIServerConfig.server_host
            resp["target_port"] = AIServerConfig.server_port
            self._send_json_response(resp)
        except Exception as e:
            self._send_json_response({
                "type": "pong",
                "status": "offline",
                "error": str(e),
                "target_host": AIServerConfig.server_host,
                "target_port": AIServerConfig.server_port,
                "model": "Not connected",
                "n_ctx": 0,
                "used_ctx": 0
            }, status=200)

    def _handle_context(self):
        client = self._get_client()
        try:
            resp = client.send_request({"type": "context"})
            self._send_json_response(resp)
        except Exception as e:
            self._send_error_json(f"Failed to query server context: {e}", 503)

    def _handle_reset(self):
        client = self._get_client()
        try:
            resp = client.send_request({"type": "reset"})
            self._send_json_response(resp)
        except Exception as e:
            self._send_error_json(f"Failed to reset context: {e}", 503)

    def _handle_chat(self, req_json):
        messages = req_json.get("messages", [])
        temperature = float(req_json.get("temperature", 0.7))
        stream = bool(req_json.get("stream", True))

        if not messages or not isinstance(messages, list):
            self._send_error_json("Missing or invalid 'messages' array", 400)
            return

        client = self._get_client()

        if not stream:
            # Synchronous request
            try:
                full_resp = ""
                n_ctx = 0
                used_ctx = 0
                for event in client.stream_chat(messages, temperature=temperature):
                    if event.get("type") == "token":
                        full_resp += event.get("piece", "")
                    elif event.get("type") == "done":
                        n_ctx = event.get("n_ctx", 0)
                        used_ctx = event.get("used_ctx", 0)
                    elif event.get("type") == "error":
                        self._send_error_json(event.get("message", "Server error"), 500)
                        return
                self._send_json_response({
                    "type": "done",
                    "response": full_resp,
                    "n_ctx": n_ctx,
                    "used_ctx": used_ctx
                })
            except Exception as e:
                self._send_error_json(f"Chat request failed: {e}", 503)
            return

        # SSE Streaming
        try:
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream; charset=utf-8")
            self.send_header("Cache-Control", "no-cache")
            self.send_header("Connection", "close")
            self.send_header("Access-Control-Allow-Origin", "*")
            self.end_headers()

            for event in client.stream_chat(messages, temperature=temperature):
                event_data = f"data: {json.dumps(event)}\n\n"
                self.wfile.write(event_data.encode("utf-8"))
                self.wfile.flush()

        except Exception as e:
            try:
                err_event = f"data: {json.dumps({'type': 'error', 'message': str(e)})}\n\n"
                self.wfile.write(err_event.encode("utf-8"))
                self.wfile.flush()
            except Exception:
                pass
        finally:
            self.close_connection = True

    def _handle_generate(self, req_json):
        prompt = req_json.get("prompt", "")
        temperature = float(req_json.get("temperature", 0.7))
        stream = bool(req_json.get("stream", True))

        if not prompt:
            self._send_error_json("Missing 'prompt' string", 400)
            return

        client = self._get_client()

        if not stream:
            try:
                full_resp = ""
                n_ctx = 0
                used_ctx = 0
                for event in client.stream_generate(prompt, temperature=temperature):
                    if event.get("type") == "token":
                        full_resp += event.get("piece", "")
                    elif event.get("type") == "done":
                        n_ctx = event.get("n_ctx", 0)
                        used_ctx = event.get("used_ctx", 0)
                    elif event.get("type") == "error":
                        self._send_error_json(event.get("message", "Server error"), 500)
                        return
                self._send_json_response({
                    "type": "done",
                    "response": full_resp,
                    "n_ctx": n_ctx,
                    "used_ctx": used_ctx
                })
            except Exception as e:
                self._send_error_json(f"Generate request failed: {e}", 503)
            return

        # SSE Streaming
        try:
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream; charset=utf-8")
            self.send_header("Cache-Control", "no-cache")
            self.send_header("Connection", "close")
            self.send_header("Access-Control-Allow-Origin", "*")
            self.end_headers()

            for event in client.stream_generate(prompt, temperature=temperature):
                event_data = f"data: {json.dumps(event)}\n\n"
                self.wfile.write(event_data.encode("utf-8"))
                self.wfile.flush()

        except Exception as e:
            try:
                err_event = f"data: {json.dumps({'type': 'error', 'message': str(e)})}\n\n"
                self.wfile.write(err_event.encode("utf-8"))
                self.wfile.flush()
            except Exception:
                pass
        finally:
            self.close_connection = True


class ThreadedHTTPServer(http.server.ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = True


def main():
    parser = argparse.ArgumentParser(description="Local AI Web UI Server")
    parser.add_argument("--port", "-p", type=int, default=DEFAULT_WEB_PORT, help="Port to listen for web clients (default: 3000)")
    parser.add_argument("--host", "-H", type=str, default=DEFAULT_WEB_HOST, help="Host/IP to bind web server (default: 0.0.0.0)")
    parser.add_argument("--server-host", "-sh", type=str, default=DEFAULT_AI_SERVER_HOST, help="Target Local AI TCP server host (default: 127.0.0.1)")
    parser.add_argument("--server-port", "-sp", type=int, default=DEFAULT_AI_SERVER_PORT, help="Target Local AI TCP server port (default: 8080)")

    args = parser.parse_args()

    AIServerConfig.server_host = args.server_host
    AIServerConfig.server_port = args.server_port

    if not os.path.exists(STATIC_DIR):
        os.makedirs(STATIC_DIR, exist_ok=True)

    server = ThreadedHTTPServer((args.host, args.port), WebRequestHandler)
    print(f"==================================================")
    print(f"  🚀 Local AI Web UI started")
    print(f"  🌐 Web Address:        http://{args.host}:{args.port}")
    if args.host == "0.0.0.0":
        print(f"  👉 Local access:       http://localhost:{args.port}")
    print(f"  🤖 Target AI Server:   {AIServerConfig.server_host}:{AIServerConfig.server_port}")
    print(f"==================================================")

    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\n[web] Shutting down web server...")
        server.server_close()


if __name__ == "__main__":
    main()

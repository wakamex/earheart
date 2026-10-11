#!/usr/bin/env python3
"""A stand-in for an OpenAI-compatible service, for tests/remote.sh. It records what it was sent
in the folder given, so the test can assert on it afterwards, and answers:
  POST /v1/audio/transcriptions   {"text": ...} for a well-formed upload
  POST /v1/chat/completions       the user message back, behind a thought to be stripped
  GET  /v1/models                 a list with a duplicate, out of order
  anything under /denied/         401, with the key in the body, which no message may repeat
Usage: fake_openai.py PORT RECORD_DIR
"""
import json, socketserver, sys
from http.server import BaseHTTPRequestHandler, HTTPServer

port, record = int(sys.argv[1]), sys.argv[2]

class Handler(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def reply(self, status, body):
        data = json.dumps(body).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def seen(self, name, body):
        with open(f"{record}/{name}.json", "w") as f:
            json.dump({"path": self.path, "authorization": self.headers.get("Authorization"), "contentType": self.headers.get("Content-Type"), **body}, f)

    def do_GET(self):
        if self.path.startswith("/denied/"):
            return self.reply(401, {"error": "bad key sk-secret"})
        if self.path == "/v1/models":
            self.seen("models", {})
            return self.reply(200, {"data": [{"id": "whisper-b"}, {"id": "Whisper-a"}, {"id": "whisper-b"}, {"id": ""}]})
        self.reply(404, {})

    def do_POST(self):
        body = self.rfile.read(int(self.headers.get("Content-Length", 0)))
        if self.path.startswith("/denied/"):
            return self.reply(401, {"error": "bad key sk-secret"})
        if self.path == "/v1/audio/transcriptions":
            ok = b'name="file"; filename="audio.wav"' in body and b"RIFF" in body and b'name="response_format"' in body
            self.seen("stt", {"bytes": len(body), "wellFormed": ok, "model": b'name="model"\r\n\r\nremote-stt' in body, "language": b'name="language"\r\n\r\nen' in body})
            return self.reply(200 if ok else 400, {"text": "  um this is the the remote transcript  "})
        if self.path == "/v1/chat/completions":
            sent = json.loads(body)
            self.seen("cleanup", {"request": sent})
            user = sent["messages"][1]["content"]
            return self.reply(200, {"choices": [{"finish_reason": "stop", "message": {"content": f"<think>let me think</think>Cleaned: {user}"}}]})
        self.reply(404, {})

class Server(HTTPServer):
    # HTTPServer looks up the host's own name while binding, which takes seconds on some machines.
    def server_bind(self):
        socketserver.TCPServer.server_bind(self)
        self.server_name, self.server_port = self.server_address[:2]

Server(("127.0.0.1", port), Handler).serve_forever()

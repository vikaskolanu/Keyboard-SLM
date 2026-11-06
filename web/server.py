#!/usr/bin/env python3
import ctypes
import json
import os
import sys
import time
from http.server import HTTPServer, BaseHTTPRequestHandler
from urllib.parse import urlparse, parse_qs

# Locate and load the C++ shared library
BASE_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LIB_PATH = os.path.join(BASE_DIR, 'engine', 'libkeyboard_slm.dylib')

if not os.path.exists(LIB_PATH):
    print(f"Error: {LIB_PATH} not found. Please compile the C++ engine first.", file=sys.stderr)
    sys.exit(1)

kslm = ctypes.CDLL(LIB_PATH)

# Declare FFI function prototypes
kslm.kslm_create.restype = ctypes.c_void_p
kslm.kslm_destroy.argtypes = [ctypes.c_void_p]

kslm.kslm_learn_sentence.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_uint32]
kslm.kslm_learn_word.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_uint32]
kslm.kslm_forget_word.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
kslm.kslm_forget_word.restype = ctypes.c_int
kslm.kslm_contains_word.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
kslm.kslm_contains_word.restype = ctypes.c_int
kslm.kslm_clear.argtypes = [ctypes.c_void_p]

kslm.kslm_predict_json.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_size_t]
kslm.kslm_predict_json.restype = ctypes.c_void_p

kslm.kslm_list_senders_json.argtypes = [ctypes.c_char_p]
kslm.kslm_list_senders_json.restype = ctypes.c_void_p

kslm.kslm_import_chat.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p]
kslm.kslm_import_chat.restype = ctypes.c_size_t

kslm.kslm_get_stats_json.argtypes = [ctypes.c_void_p]
kslm.kslm_get_stats_json.restype = ctypes.c_void_p

kslm.kslm_save.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
kslm.kslm_save.restype = ctypes.c_int

kslm.kslm_load.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
kslm.kslm_load.restype = ctypes.c_int

kslm.kslm_free_string.argtypes = [ctypes.c_void_p]

# Create global model instance
engine_ptr = kslm.kslm_create()

def get_stats():
    ptr = kslm.kslm_get_stats_json(engine_ptr)
    s = ctypes.cast(ptr, ctypes.c_char_p).value.decode('utf-8')
    kslm.kslm_free_string(ptr)
    return json.loads(s)

def split_context(text):
    """
    Given typing buffer text:
    If text ends with whitespace: next-word prediction mode.
    If text does not end with whitespace: last word is current prefix being typed.
    """
    is_next_word = len(text) > 0 and text[-1].isspace()
    tokens = text.strip().split()
    if is_next_word or len(tokens) == 0:
        prefix = ""
        p1 = tokens[-1] if len(tokens) >= 1 else ""
        p2 = tokens[-2] if len(tokens) >= 2 else ""
    else:
        prefix = tokens[-1]
        p1 = tokens[-2] if len(tokens) >= 2 else ""
        p2 = tokens[-3] if len(tokens) >= 3 else ""
    return p2, p1, prefix, is_next_word

class SLMHandler(BaseHTTPRequestHandler):
    def _send_json(self, data, status=200):
        body = json.dumps(data).encode('utf-8')
        self.send_response(status)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(body)))
        self.send_header('Access-Control-Allow-Origin', '*')
        self.send_header('Access-Control-Allow-Methods', 'GET, POST, OPTIONS')
        self.send_header('Access-Control-Allow-Headers', 'Content-Type')
        self.end_headers()
        self.wfile.write(body)

    def do_OPTIONS(self):
        self.send_response(200)
        self.send_header('Access-Control-Allow-Origin', '*')
        self.send_header('Access-Control-Allow-Methods', 'GET, POST, OPTIONS')
        self.send_header('Access-Control-Allow-Headers', 'Content-Type')
        self.end_headers()

    def do_GET(self):
        url = urlparse(self.path)
        if url.path == '/' or url.path == '/index.html':
            html_path = os.path.join(os.path.dirname(__file__), 'index.html')
            if not os.path.exists(html_path):
                self.send_error(404, "index.html not found")
                return
            with open(html_path, 'rb') as f:
                content = f.read()
            self.send_response(200)
            self.send_header('Content-Type', 'text/html; charset=utf-8')
            self.send_header('Content-Length', str(len(content)))
            self.end_headers()
            self.wfile.write(content)
        elif url.path == '/api/stats':
            self._send_json(get_stats())
        elif url.path == '/api/check_downloads':
            # Check if default _chat.txt exists in Downloads
            dl_path = os.path.expanduser('~/Downloads/_chat.txt')
            dl2_path = os.path.expanduser('~/Downloads/_chat 2.txt')
            files = []
            if os.path.exists(dl_path):
                files.append({"name": "_chat.txt", "path": dl_path, "size": os.path.getsize(dl_path)})
            if os.path.exists(dl2_path):
                files.append({"name": "_chat 2.txt", "path": dl2_path, "size": os.path.getsize(dl2_path)})
            self._send_json({"found_files": files})
        else:
            self.send_error(404, "Not Found")

    def do_POST(self):
        url = urlparse(self.path)
        length = int(self.headers.get('Content-Length', 0))
        body = self.rfile.read(length).decode('utf-8') if length > 0 else "{}"
        try:
            payload = json.loads(body)
        except Exception:
            payload = {}

        if url.path == '/api/predict':
            text = payload.get('text', '')
            top_k = payload.get('top_k', 3)
            p2, p1, prefix, is_next_word = split_context(text)

            t0 = time.perf_counter()
            ptr = kslm.kslm_predict_json(
                engine_ptr,
                p2.encode('utf-8'),
                p1.encode('utf-8'),
                prefix.encode('utf-8'),
                top_k
            )
            elapsed_us = (time.perf_counter() - t0) * 1_000_000.0
            s = ctypes.cast(ptr, ctypes.c_char_p).value.decode('utf-8')
            kslm.kslm_free_string(ptr)
            preds = json.loads(s)

            self._send_json({
                "predictions": preds,
                "latency_us": round(elapsed_us, 1),
                "context": {
                    "prev2": p2,
                    "prev1": p1,
                    "prefix": prefix,
                    "is_next_word": is_next_word
                }
            })

        elif url.path == '/api/learn':
            sentence = payload.get('sentence', '')
            weight = payload.get('weight', 1)
            if sentence:
                kslm.kslm_learn_sentence(engine_ptr, sentence.encode('utf-8'), weight)
            self._send_json({"status": "ok", "stats": get_stats()})

        elif url.path == '/api/forget':
            word = payload.get('word', '')
            ok = kslm.kslm_forget_word(engine_ptr, word.encode('utf-8'))
            self._send_json({"status": "ok" if ok else "not_found", "stats": get_stats()})

        elif url.path == '/api/clear':
            kslm.kslm_clear(engine_ptr)
            self._send_json({"status": "ok", "stats": get_stats()})

        elif url.path == '/api/parse_senders':
            filepath = payload.get('filepath')
            content = payload.get('content')
            if filepath and os.path.exists(filepath):
                with open(filepath, 'r', encoding='utf-8', errors='ignore') as f:
                    content = f.read()

            if not content:
                self._send_json({"senders": []})
                return

            ptr = kslm.kslm_list_senders_json(content.encode('utf-8'))
            s = ctypes.cast(ptr, ctypes.c_char_p).value.decode('utf-8')
            kslm.kslm_free_string(ptr)
            self._send_json({"senders": json.loads(s)})

        elif url.path == '/api/import_chat':
            filepath = payload.get('filepath')
            content = payload.get('content')
            sender = payload.get('sender', '')

            if filepath and os.path.exists(filepath):
                with open(filepath, 'r', encoding='utf-8', errors='ignore') as f:
                    content = f.read()

            if not content:
                self._send_json({"imported": 0, "stats": get_stats()})
                return

            t0 = time.perf_counter()
            imported = kslm.kslm_import_chat(
                engine_ptr,
                content.encode('utf-8'),
                sender.encode('utf-8')
            )
            elapsed_ms = (time.perf_counter() - t0) * 1000.0

            self._send_json({
                "imported": imported,
                "elapsed_ms": round(elapsed_ms, 1),
                "stats": get_stats()
            })

        elif url.path == '/api/save':
            filepath = payload.get('filepath', os.path.join(BASE_DIR, 'personal_model.bin'))
            ok = kslm.kslm_save(engine_ptr, filepath.encode('utf-8'))
            size_kb = os.path.getsize(filepath) / 1024.0 if ok and os.path.exists(filepath) else 0
            self._send_json({"saved": bool(ok), "filepath": filepath, "size_kb": round(size_kb, 1)})

        elif url.path == '/api/load':
            filepath = payload.get('filepath', os.path.join(BASE_DIR, 'personal_model.bin'))
            ok = kslm.kslm_load(engine_ptr, filepath.encode('utf-8'))
            self._send_json({"loaded": bool(ok), "filepath": filepath, "stats": get_stats()})

        else:
            self.send_error(404, "Not Found")

def run(port=8080):
    server_address = ('127.0.0.1', port)
    try:
        httpd = HTTPServer(server_address, SLMHandler)
    except OSError:
        server_address = ('127.0.0.1', port + 1)
        httpd = HTTPServer(server_address, SLMHandler)
        port = port + 1

    print(f"[*] Keyboard SLM Server running at http://localhost:{port}")
    sys.stdout.flush()
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        kslm.kslm_destroy(engine_ptr)
        print("[*] Server stopped.")

if __name__ == '__main__':
    p = int(sys.argv[1]) if len(sys.argv) > 1 else 8080
    run(p)

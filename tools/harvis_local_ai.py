#!/usr/bin/env python3
"""HARVIS local AI bridge.

This service exposes an Ollama-compatible JSON API so the kernel's existing
agent idiom can target a local, offline AI backend instead of a cloud service.
It works even when no model runtime is installed by returning a deterministic
response that still follows the same request/response contract.
"""

from __future__ import annotations

import json
import os
import subprocess
import sys
from datetime import datetime, timezone
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from typing import Any, Dict, Optional

DEFAULT_HOST = os.environ.get("HARVIS_AI_HOST", "127.0.0.1")
DEFAULT_PORT = int(os.environ.get("HARVIS_AI_PORT", "11434"))
DEFAULT_MODEL = os.environ.get("HARVIS_MODEL", "harvis-local")
OLLAMA_BIN = os.environ.get("OLLAMA_BIN", "ollama")
FORCE_FALLBACK = os.environ.get("HARVIS_AI_FORCE_FALLBACK", "").lower() in {
    "1", "true", "yes", "on"
}


def resolve_model_name(raw_name: Optional[str]) -> str:
    cleaned = (raw_name or "").strip()
    return cleaned or DEFAULT_MODEL


def build_response(payload: Dict[str, Any]) -> Dict[str, Any]:
    model_name = resolve_model_name(payload.get("model"))
    system_prompt = str(payload.get("system") or "JARVIS local mode")
    prompt_text = str(payload.get("prompt") or "status")

    prompt_lower = prompt_text.lower()
    if "[agent exec result]" in prompt_lower:
        response_text = "<EXEC_CMD:stop>"
    elif "[agent task]" in prompt_lower:
        if any(word in prompt_lower for word in ("sysinfo", "status", "health")):
            response_text = "<EXEC_CMD:sysinfo>"
        else:
            response_text = "<EXEC_CMD:ls />"
    else:
        if "ls" in prompt_lower or "list" in prompt_lower or "dir" in prompt_lower:
            action_line = "Suggested action: list the available workspace and summarize the active environment."
        elif "sysinfo" in prompt_lower or "status" in prompt_lower or "health" in prompt_lower:
            action_line = "Suggested action: inspect the system state, tasks, memory, and filesystem health."
        elif "fix" in prompt_lower or "debug" in prompt_lower or "diagnose" in prompt_lower:
            action_line = "Suggested action: inspect the relevant logs, identify the failing component, and propose the lowest-risk fix."
        else:
            action_line = "Suggested action: interpret the request, maintain local context, and plan bounded actions."

        response_text = (
            f"[HARVIS LOCAL AI]\n"
            f"Model: {model_name}\n"
            f"System: {system_prompt}\n"
            f"Prompt: {prompt_text}\n\n"
            f"I am running in offline local-only mode. I can inspect the system, summarize state, and plan bounded actions.\n"
            f"{action_line}\n"
            "The environment remains safe by default: no cloud call, no unbounded destructive action, and all context is kept local."
        )

    return {
        "model": model_name,
        "created_at": datetime.now(timezone.utc).isoformat(),
        "response": response_text,
        "done": True,
        "context": {
            "system": system_prompt,
            "prompt": prompt_text,
        },
    }


def maybe_call_ollama(payload: Dict[str, Any]) -> Optional[Dict[str, Any]]:
    """Attempt to use a real Ollama installation if present.

    If not present, fall back to the deterministic local harness response.
    """
    if FORCE_FALLBACK or not shutil_which(OLLAMA_BIN):
        return None

    try:
        prompt_text = str(payload.get("prompt") or "status")
        system_prompt = str(payload.get("system") or "JARVIS local mode")
        model_name = resolve_model_name(payload.get("model"))
        proc = subprocess.run(
            [OLLAMA_BIN, "run", model_name, f"{system_prompt}\n\n{prompt_text}"],
            capture_output=True,
            text=True,
            timeout=20,
            check=False,
        )
        if proc.returncode == 0 and proc.stdout.strip():
            return {
                "model": model_name,
                "created_at": datetime.now(timezone.utc).isoformat(),
                "response": proc.stdout.strip(),
                "done": True,
            }
    except Exception:
        pass
    return None


def shutil_which(binary_name: str) -> Optional[str]:
    for path in os.environ.get("PATH", "").split(os.pathsep):
        candidate = os.path.join(path, binary_name)
        if os.path.isfile(candidate) and os.access(candidate, os.X_OK):
            return candidate
    return None


def handle_generate(payload: Dict[str, Any]) -> Dict[str, Any]:
    response = maybe_call_ollama(payload)
    if response is not None:
        return response
    return build_response(payload)


class HARVISHandler(BaseHTTPRequestHandler):
    server_version = "HARVISLocalAI/0.1"

    def do_GET(self) -> None:  # noqa: N802
        if self.path in ("/", "/health"):
            self._send_json({"ok": True, "service": "HARVIS local AI bridge"})
            return
        if self.path == "/api/tags":
            self._send_json({"models": [{"name": DEFAULT_MODEL}]})
            return
        self._send_json({"error": "not found"}, status=404)

    def do_POST(self) -> None:  # noqa: N802
        if self.path != "/api/generate":
            self._send_json({"error": "endpoint not found"}, status=404)
            return

        try:
            length = int(self.headers.get("Content-Length", "0"))
            body = self.rfile.read(length) if length > 0 else b"{}"
            payload = json.loads(body.decode("utf-8") or "{}")
            if not isinstance(payload, dict):
                raise ValueError("JSON payload must be an object")
            result = handle_generate(payload)
            self._send_json(result)
        except Exception as exc:  # pragma: no cover - surface useful diagnostics
            self._send_json({"error": str(exc)}, status=400)

    def _send_json(self, payload: Dict[str, Any], status: int = 200) -> None:
        body = json.dumps(payload, separators=(",", ":")).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, format: str, *args: Any) -> None:  # noqa: A003
        return


def run_server(host: str = DEFAULT_HOST, port: int = DEFAULT_PORT) -> None:
    server = ThreadingHTTPServer((host, port), HARVISHandler)
    print(f"HARVIS local AI bridge listening on http://{host}:{port}")
    server.serve_forever()


if __name__ == "__main__":
    try:
        run_server(DEFAULT_HOST, DEFAULT_PORT)
    except KeyboardInterrupt:
        print("\nHARVIS local AI bridge stopped.")
        sys.exit(0)

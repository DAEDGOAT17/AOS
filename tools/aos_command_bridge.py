#!/usr/bin/env python3
"""Submit a goal to AOS's native agent over the direct-link TCP shell.

The host transports one goal only. AOS owns planning, tool policy, execution,
and verification. Model/command helpers remain available for diagnostics.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import socket
import sys
import urllib.request
from typing import Optional

DEFAULT_MODEL = os.environ.get("HARVIS_MODEL", "gemma3:4b")
DEFAULT_AOS_HOST = os.environ.get("AOS_HOST", "192.168.1.50")
DEFAULT_AOS_PORT = int(os.environ.get("AOS_PORT", "9001"))
DEFAULT_OLLAMA_URL = os.environ.get("OLLAMA_URL", "http://127.0.0.1:11434/api/generate")

SAFE_COMMANDS = {
    "ls", "cat", "pwd", "help", "ps", "mem", "sysinfo", "cpuid", "arch",
    "ifconfig", "netstat", "ping", "clear", "echo", "voice", "agent_ctx_get",
    "agent_ctx_set", "agent_plan", "agent_selfcheck", "agent_task", "agent_complete",
    "pktdump", "ai_mock",
}


def is_safe_command(command: str) -> bool:
    text = (command or "").strip()
    if not text or len(text) > 256:
        return False
    if re.search(r"(?:;|&&|\|\||`|>|<)", text):
        return False
    if re.search(r"\b(?:reboot|shutdown|poweroff|mkfs|dd|sudo|rm\s+-rf|kill\s+-9)\b", text, re.I):
        return False

    first = text.split()[0].lower()
    return first in SAFE_COMMANDS


def extract_command(response: str) -> Optional[str]:
    if not response:
        return None
    cleaned = response.strip()
    cleaned = re.sub(r"```.*?```", " ", cleaned, flags=re.S)
    cleaned = cleaned.replace("\r", " ")

    tag_match = re.search(r"<EXEC_CMD:\s*([^>]+)>", cleaned, flags=re.I)
    if tag_match:
        candidate = tag_match.group(1).strip()
        return candidate if is_safe_command(candidate) else None

    first = cleaned.splitlines()[0].strip() if cleaned.splitlines() else ""
    if first and is_safe_command(first):
        return first
    return cleaned if cleaned and is_safe_command(cleaned) else None


def call_ollama(model: str, prompt: str, endpoint: str = DEFAULT_OLLAMA_URL) -> str:
    payload = json.dumps({
        "model": model,
        "prompt": prompt,
        "stream": False,
        "system": "Return one safe AOS diagnostic command in <EXEC_CMD:...> tags.",
    }).encode("utf-8")
    request = urllib.request.Request(endpoint, data=payload,
                                     headers={"Content-Type": "application/json"}, method="POST")
    with urllib.request.urlopen(request, timeout=30) as response:
        data = json.loads(response.read().decode("utf-8"))
    return str(data.get("response", "")).strip()


def send_to_aos(host: str, port: int, command: str, timeout: float = 5.0) -> str:
    with socket.create_connection((host, port), timeout=timeout) as sock:
        sock.sendall((command.strip() + "\n").encode("utf-8"))
        chunk = bytearray()
        sock.settimeout(timeout)
        while True:
            try:
                part = sock.recv(4096)
            except socket.timeout:
                break
            if not part:
                break
            chunk.extend(part)
    return chunk.decode("utf-8", errors="replace")


def submit_task(host: str, port: int, goal: str, timeout: float = 5.0) -> str:
    text = (goal or "").strip()
    if not text or len(text) > 220 or any(ord(character) < 32 or ord(character) == 127 for character in text):
        raise ValueError("goal must be one line of 1 to 220 printable characters")
    return send_to_aos(host, port, "ask " + text, timeout=timeout)


def main() -> int:
    parser = argparse.ArgumentParser(description="Submit a task to the AOS-native agent")
    parser.add_argument("--host", default=DEFAULT_AOS_HOST, help="AOS target host")
    parser.add_argument("--port", type=int, default=DEFAULT_AOS_PORT, help="AOS target port")
    parser.add_argument("--goal", required=True, help="Natural-language task for AOS to perform")
    args = parser.parse_args()
    try:
        print(submit_task(args.host, args.port, args.goal))
    except (OSError, ValueError) as error:
        print(f"Could not submit task to AOS: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())

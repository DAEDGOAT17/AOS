#!/usr/bin/env python3
"""Local command bridge for AOS.

This script runs on the LOQ machine and lets a local Ollama model pick a safe,
read-only shell command for AOS. The model output is normalized to a single AOS
command and then sent to the remote AOS shell over TCP.
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
    "ls",
    "cat",
    "pwd",
    "help",
    "ps",
    "mem",
    "sysinfo",
    "cpuid",
    "arch",
    "ifconfig",
    "netstat",
    "ping",
    "clear",
    "echo",
    "voice",
    "agent_ctx_get",
    "agent_ctx_set",
    "agent_plan",
    "agent_selfcheck",
    "agent_task",
    "agent_complete",
    "pktdump",
    "ai_mock",
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
    if first in SAFE_COMMANDS:
        return True

    if first in {"ls", "cat", "ping", "agent_ctx_get", "agent_ctx_set", "agent_plan", "agent_task", "voice", "pktdump", "echo"}:
        return True

    return False


def extract_command(response: str) -> Optional[str]:
    if not response:
        return None
    cleaned = response.strip()
    cleaned = re.sub(r"```.*?```", " ", cleaned, flags=re.S)
    cleaned = cleaned.replace("\r", " ")

    tag_match = re.search(r"<EXEC_CMD:\s*([^>]+)>", cleaned, flags=re.I)
    if tag_match:
        candidate = tag_match.group(1).strip()
        if is_safe_command(candidate):
            return candidate
        return None

    command_match = re.search(r"(?i)\b(?:command|run|execute)\b\s*[:=]\s*`?([A-Za-z0-9_./\- ]+)`?", cleaned)
    if command_match:
        candidate = command_match.group(1).strip()
        if is_safe_command(candidate):
            return candidate
        return None

    first = cleaned.splitlines()[0].strip() if cleaned.splitlines() else ""
    if first and is_safe_command(first):
        return first

    if cleaned and is_safe_command(cleaned):
        return cleaned

    return None


def call_ollama(model: str, prompt: str, endpoint: str = DEFAULT_OLLAMA_URL) -> str:
    payload = json.dumps({
        "model": model,
        "prompt": prompt,
        "stream": False,
        "system": (
            "You are the local OS command planner for AOS. "
            "Return exactly one safe command. "
            "Allowed commands: ls, cat, sysinfo, ps, mem, cpuid, arch, help, ifconfig, netstat, ping, clear, echo, voice status, agent_ctx_get, agent_ctx_set, agent_plan, agent_selfcheck, agent_task, agent_complete. "
            "Do not use shell chaining or destructive commands. "
            "Wrap the command in <EXEC_CMD:...> tags."
        ),
    }).encode("utf-8")

    request = urllib.request.Request(endpoint, data=payload, headers={"Content-Type": "application/json"}, method="POST")
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


def run_loop(host: str = DEFAULT_AOS_HOST, port: int = DEFAULT_AOS_PORT, model: str = DEFAULT_MODEL, iterations: int = 3) -> int:
    context = "You are operating a minimal AOS shell. Prefer read-only status commands and keep output concise."
    for step in range(iterations):
        prompt = (
            f"{context}\n"
            "Pick two safe command for the OS. "
            "and to then make them run"
            "Return only the command in <EXEC_CMD:...> tags. "
            "Do not include explanation."
        )
        response = call_ollama(model, prompt)
        command = extract_command(response)
        if not command:
            print(f"No safe command extracted from model output: {response}")
            return 1
        print(f"Step {step + 1}: {command}")
        result = send_to_aos(host, port, command)
        print(result)
        if "stop" in result.lower() or "done" in result.lower():
            return 0
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description="Run the local LOQ-to-AOS command relay")
    parser.add_argument("--host", default=DEFAULT_AOS_HOST, help="AOS target host")
    parser.add_argument("--port", type=int, default=DEFAULT_AOS_PORT, help="AOS TCP port")
    parser.add_argument("--model", default=DEFAULT_MODEL, help="Ollama model name")
    parser.add_argument("--iterations", type=int, default=3, help="Maximum command loop iterations")
    args = parser.parse_args()
    return run_loop(host=args.host, port=args.port, model=args.model, iterations=args.iterations)


if __name__ == "__main__":
    sys.exit(main())

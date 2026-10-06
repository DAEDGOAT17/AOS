#!/usr/bin/env python3
"""HARVIS end-to-end agent loop.

This prototype wires together the local AI bridge and the self-upgrade planner,
so a command can produce a plan, fetch an AI response, and save a combined JSON
artifact for later execution and validation.
"""

from __future__ import annotations

import json
import os
import sys
import urllib.request
from typing import Any, Dict, Optional

THIS_DIR = os.path.dirname(os.path.abspath(__file__))
if THIS_DIR not in sys.path:
    sys.path.insert(0, THIS_DIR)

from harvis_self_upgrade import plan_self_improvement, save_plan


def call_local_ai(prompt: str, endpoint: str = "http://127.0.0.1:11434/api/generate", model: str = "harvis-local", system: str = "local mode") -> Dict[str, Any]:
    payload = {
        "model": model,
        "system": system,
        "prompt": prompt,
        "stream": False,
    }
    data = json.dumps(payload).encode("utf-8")
    request = urllib.request.Request(
        endpoint,
        data=data,
        headers={"Content-Type": "application/json", "User-Agent": "HARVIS-Agent/0.1"},
        method="POST",
    )
    try:
        with urllib.request.urlopen(request, timeout=10) as response:
            body = response.read().decode("utf-8", errors="replace")
            parsed = json.loads(body)
            return {
                "status": "ok",
                "response": parsed.get("response", body),
                "raw": parsed,
            }
    except Exception:
        return {
            "status": "fallback",
            "response": (
                "[HARVIS LOCAL AI]\n"
                "Offline local-only fallback.\n"
                f"Prompt: {prompt}\n"
                "I can inspect the runtime, propose a plan, and keep the action bounded."
            ),
            "raw": {},
        }


def save_agent_cycle(payload: Dict[str, Any], path: str) -> str:
    target = path or os.path.join(os.getcwd(), "harvis_agent_cycle.json")
    with open(target, "w", encoding="utf-8") as fh:
        json.dump(payload, fh, indent=2, sort_keys=True)
        fh.write("\n")
    return target


def ensure_db_root(db_root: Optional[str] = None) -> str:
    target = db_root or os.path.join(os.getcwd(), "agent_db")
    os.makedirs(target, exist_ok=True)
    return target


def save_context_entry(key: str, value: str, db_root: Optional[str] = None) -> str:
    root = ensure_db_root(db_root)
    safe_key = (key or "agent").strip().replace("/", "_")
    path = os.path.join(root, f"{safe_key}.txt")
    with open(path, "w", encoding="utf-8") as fh:
        fh.write(str(value))
        fh.write("\n")
    return path


def run_agent_cycle(goal: str, context: str, prompt: Optional[str] = None, endpoint: str = "http://127.0.0.1:11434/api/generate", out_path: Optional[str] = None, db_root: Optional[str] = None) -> Dict[str, Any]:
    plan = plan_self_improvement(goal, context)
    ai_response = call_local_ai(prompt or "status", endpoint=endpoint)
    cycle = {
        "goal": goal,
        "context": context,
        "plan": plan,
        "ai_response": ai_response.get("response", ""),
        "ai_status": ai_response.get("status", "fallback"),
        "raw_ai": ai_response.get("raw", {}),
    }
    if out_path is not None:
        save_agent_cycle(cycle, out_path)

    if db_root is not None or True:
        db_target = ensure_db_root(db_root)
        context_bundle = json.dumps(cycle, indent=2, sort_keys=True)
        save_context_entry("self_improvement", context_bundle, db_root=db_target)

    return cycle


if __name__ == "__main__":
    import argparse

    parser = argparse.ArgumentParser(description="Run a HARVIS agent cycle")
    parser.add_argument("--goal", default="Make the OS self-improving", help="Goal for the agent cycle")
    parser.add_argument("--context", default="Kernel and agent memory are present", help="Current system context")
    parser.add_argument("--prompt", default="status", help="Prompt to send to the local AI bridge")
    parser.add_argument("--endpoint", default="http://127.0.0.1:11434/api/generate", help="Local AI endpoint")
    parser.add_argument("--out", default="harvis_agent_cycle.json", help="Path to save the combined plan and AI result")
    parser.add_argument("--db-root", default=None, help="Directory to store the persistent agent context entries")
    args = parser.parse_args()

    result = run_agent_cycle(args.goal, args.context, prompt=args.prompt, endpoint=args.endpoint, out_path=args.out, db_root=args.db_root)
    print(json.dumps(result, indent=2, sort_keys=True))

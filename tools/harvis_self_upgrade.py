#!/usr/bin/env python3
"""HARVIS self-improvement planner.

This is the first bounded self-upgrade loop for the project: it can fetch an
external reference, turn it into a structured plan, and save the result as a
JSON plan file for later execution and validation.
"""

from __future__ import annotations

import json
import os
import urllib.request
from typing import Any, Dict, Optional


def plan_self_improvement(goal: str, context: str) -> Dict[str, Any]:
    """Create a bounded plan for developing the OS toward a goal."""
    target_goal = (goal or "Build a more autonomous agentic OS").strip()
    target_context = (context or "Bootable kernel, shell, and agent memory").strip()
    steps = [
        "Inspect the current kernel, shell, and agent subsystem state.",
        "Capture persistent memory and task context needed for the requested capability.",
        "Identify the minimum safe change required to enable the new behavior.",
        "Write the patch or new module in a staging area and keep it isolated from the boot path.",
        "Validate the change with a focused real-world test before applying it.",
        "Persist the result and update the agent memory with the outcome.",
    ]
    return {
        "goal": target_goal,
        "context": target_context,
        "steps": steps,
        "validation": "Run targeted tests and ensure the bootable OS still reaches the interactive prompt.",
    }


def save_plan(plan: Dict[str, Any], path: str) -> str:
    """Save a plan dictionary to a JSON file and return the path."""
    target = path or os.path.join(os.getcwd(), "harvis_plan.json")
    with open(target, "w", encoding="utf-8") as fh:
        json.dump(plan, fh, indent=2, sort_keys=True)
        fh.write("\n")
    return target


def fetch_url_text(url: str) -> Optional[str]:
    """Fetch text content from a URL when available.

    The function is intentionally conservative: it only fetches text and returns
    None on network errors or invalid URLs.
    """
    if not url or not url.strip():
        return None
    request = urllib.request.Request(url, headers={"User-Agent": "HARVIS-Agent/0.1"})
    try:
        with urllib.request.urlopen(request, timeout=10) as response:
            content_type = response.headers.get_content_type()
            if content_type and "text" not in content_type and "json" not in content_type and "xml" not in content_type:
                return None
            payload = response.read()
            return payload.decode("utf-8", errors="replace")
    except Exception:
        return None


def run_self_upgrade_cycle(goal: str, context: str, url: Optional[str] = None) -> Dict[str, Any]:
    """Create a plan and optionally enrich it with a fetched external reference."""
    plan = plan_self_improvement(goal, context)
    if url:
        fetched = fetch_url_text(url)
        if fetched is not None:
            plan["external_reference"] = {
                "url": url,
                "snippet": fetched[:2000],
            }
        else:
            plan["external_reference"] = {"url": url, "snippet": "unable to fetch"}
    return plan


if __name__ == "__main__":
    import argparse

    parser = argparse.ArgumentParser(description="Create a bounded HARVIS self-improvement plan")
    parser.add_argument("--goal", default="Make the OS agentic and internet-aware", help="What the OS should become")
    parser.add_argument("--context", default="Bootable kernel with agent memory and shell commands", help="Current state of the system")
    parser.add_argument("--url", default=None, help="Optional external reference URL for research")
    parser.add_argument("--out", default="harvis_plan.json", help="Output path for the plan JSON")
    args = parser.parse_args()

    plan = run_self_upgrade_cycle(args.goal, args.context, args.url)
    saved = save_plan(plan, args.out)
    print(json.dumps({"saved": saved, "plan": plan}, indent=2))

"""Native-style local inference runtime for the AOS agentic OS.

This is a safe, deterministic system component that behaves like a tiny embedded
inference layer: it accepts OS state and a user prompt, emits a typed decision,
and enforces an action allowlist before any command can be executed.

The design deliberately avoids unrestricted text generation. Instead, it returns
structured decisions and confidence values for the OS to act on.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from enum import Enum
import re
from typing import Any, Dict, Iterable, Optional


class DecisionAction(str, Enum):
    NONE = "none"
    HELP = "help"
    SHOW_STATUS = "show_status"
    LIST_FILES = "list_files"
    CLEAR_SCREEN = "clear_screen"
    REBOOT = "reboot"
    REVIEW_PATCH = "review_patch"
    ALERT_OPERATOR = "alert_operator"
    STOP_TASK = "stop_task"
    PATCH_CONFIG = "patch_config"


@dataclass
class InferResult:
    action: DecisionAction
    confidence: float
    reason: str
    safe: bool
    metadata: Dict[str, Any] = field(default_factory=dict)


@dataclass
class LLMActionDecision:
    """Structured inference output meant for the kernel control plane."""
    action: DecisionAction
    target: str = "kernel"
    arguments: Dict[str, Any] = field(default_factory=dict)
    confidence: float = 0.0
    reason: str = ""
    safe: bool = True
    requires_review: bool = False
    source: str = "local_runtime"


class CapabilityGate:
    """Enforces the action policy that gates model decisions to kernel execution."""

    REVIEW_REQUIRED = {
        DecisionAction.REBOOT,
        DecisionAction.PATCH_CONFIG,
        DecisionAction.STOP_TASK,
        DecisionAction.REVIEW_PATCH,
    }

    def __init__(self, allowlist: Optional[Iterable[DecisionAction]] = None) -> None:
        self.allowlist = set(allowlist) if allowlist is not None else set(
            {
                DecisionAction.NONE,
                DecisionAction.HELP,
                DecisionAction.SHOW_STATUS,
                DecisionAction.LIST_FILES,
                DecisionAction.CLEAR_SCREEN,
                DecisionAction.REBOOT,
                DecisionAction.REVIEW_PATCH,
                DecisionAction.ALERT_OPERATOR,
                DecisionAction.STOP_TASK,
                DecisionAction.PATCH_CONFIG,
            }
        )

    def can_execute(self, decision: LLMActionDecision, reviewed: bool = False) -> bool:
        if decision.action not in self.allowlist:
            return False
        if decision.action in self.REVIEW_REQUIRED and not reviewed:
            decision.requires_review = True
            decision.safe = False
            return False
        decision.requires_review = decision.action in self.REVIEW_REQUIRED
        return True


class LocalOSInferenceRuntime:
    """A bounded, deterministic decisioning layer for the OS control plane."""

    ALLOWLIST = {
        DecisionAction.NONE,
        DecisionAction.HELP,
        DecisionAction.SHOW_STATUS,
        DecisionAction.LIST_FILES,
        DecisionAction.CLEAR_SCREEN,
        DecisionAction.REBOOT,
        DecisionAction.REVIEW_PATCH,
        DecisionAction.ALERT_OPERATOR,
        DecisionAction.STOP_TASK,
        DecisionAction.PATCH_CONFIG,
    }

    def __init__(self, allowlist: Optional[Iterable[DecisionAction]] = None) -> None:
        self.allowlist = set(allowlist) if allowlist is not None else set(self.ALLOWLIST)

    @staticmethod
    def normalize_text(value: Optional[str]) -> str:
        if value is None:
            return ""
        cleaned = value.lower()
        cleaned = re.sub(r"[^a-z0-9\s]", " ", cleaned)
        cleaned = re.sub(r"\s+", " ", cleaned).strip()
        return cleaned

    @staticmethod
    def score_keywords(text: str, keywords: Iterable[str]) -> float:
        tokens = set(text.split())
        return 1.0 if any(keyword in text for keyword in keywords) else 0.0

    def validate_action(self, action: DecisionAction) -> bool:
        return action in self.allowlist

    def route_direct_action(self, decision: InferResult) -> str:
        """Resolve a typed decision to the direct kernel action command."""
        if decision is None:
            return "echo no safe action matched"
        if not getattr(decision, "safe", False):
            return "echo blocked by allowlist"

        routing = {
            DecisionAction.NONE: "echo no safe action matched",
            DecisionAction.HELP: "help",
            DecisionAction.SHOW_STATUS: "sysinfo",
            DecisionAction.LIST_FILES: "ls /",
            DecisionAction.CLEAR_SCREEN: "clear",
            DecisionAction.REBOOT: "reboot",
            DecisionAction.REVIEW_PATCH: "agent_selfcheck",
            DecisionAction.ALERT_OPERATOR: "agent_ctx_set alert status",
            DecisionAction.STOP_TASK: "agent_selfcheck",
            DecisionAction.PATCH_CONFIG: "agent_plan improve",
        }
        return routing.get(decision.action, "echo no safe action matched")

    def infer(self, prompt: Optional[str], system_state: Optional[Dict[str, Any]] = None) -> InferResult:
        """Infer a safe decision given a prompt and the running OS state."""
        system_state = system_state or {}
        text = self.normalize_text(prompt)

        if not text and not system_state:
            return InferResult(
                action=DecisionAction.NONE,
                confidence=0.0,
                reason="no prompt and no OS state available",
                safe=True,
                metadata={"source": "empty"},
            )

        memory_pressure = float(system_state.get("memory_pressure", 0.0))
        health = float(system_state.get("health", 1.0))
        boot_health = float(system_state.get("boot_health", 1.0))

        if "help" in text or "what can you do" in text or "show help" in text or "commands" in text:
            action = DecisionAction.HELP
            confidence = 0.94
            reason = "help request matched the safe shell help action"
        elif memory_pressure > 0.9:
            action = DecisionAction.ALERT_OPERATOR
            confidence = 0.92
            reason = "high memory pressure detected in OS telemetry"
        elif "reboot" in text or "restart" in text:
            action = DecisionAction.REBOOT
            confidence = 0.91
            reason = "user requested a reboot in the local request"
        elif "status" in text or "sysinfo" in text or "health" in text or "state of the machine" in text or "machine state" in text:
            action = DecisionAction.SHOW_STATUS
            confidence = 0.89
            reason = "status query matched the local command grammar"
        elif "list" in text and ("file" in text or "directory" in text):
            action = DecisionAction.LIST_FILES
            confidence = 0.87
            reason = "directory listing intent was detected"
        elif "clear" in text and ("screen" in text or "terminal" in text):
            action = DecisionAction.CLEAR_SCREEN
            confidence = 0.85
            reason = "screen management command was detected"
        elif health < 0.45 or boot_health < 0.5:
            action = DecisionAction.REVIEW_PATCH
            confidence = 0.94
            reason = "system health is below the safe operational threshold"
        elif "stop" in text or "halt" in text:
            action = DecisionAction.STOP_TASK
            confidence = 0.82
            reason = "stop or halt intent was detected"
        elif "patch" in text or "improve" in text:
            action = DecisionAction.PATCH_CONFIG
            confidence = 0.78
            reason = "improvement request matched the bounded patch policy"
        else:
            action = DecisionAction.NONE
            confidence = 0.17
            reason = "no recognized safe action; request will be transcribed or held"

        safe = self.validate_action(action)
        result = InferResult(
            action=action,
            confidence=float(min(max(confidence, 0.0), 1.0)),
            reason=reason,
            safe=safe,
            metadata={
                "raw_prompt": text,
                "memory_pressure": memory_pressure,
                "system_health": health,
                "boot_health": boot_health,
                "allowlisted": safe,
            },
        )
        return result


class LocalGGUFAdapter:
    """A local GGUF-style adapter interface that converts prompts into typed actions."""

    def __init__(self, runtime: Optional[LocalOSInferenceRuntime] = None, gate: Optional[CapabilityGate] = None) -> None:
        self.runtime = runtime or LocalOSInferenceRuntime()
        self.gate = gate or CapabilityGate(self.runtime.ALLOWLIST)

    def decide(self, prompt: Optional[str], system_state: Optional[Dict[str, Any]] = None, reviewed: bool = False) -> LLMActionDecision:
        decision = self.runtime.infer(prompt, system_state)
        llm_decision = LLMActionDecision(
            action=decision.action,
            target="kernel",
            arguments={"prompt": prompt or "", "system_state": system_state or {}},
            confidence=decision.confidence,
            reason=decision.reason,
            safe=decision.safe,
            requires_review=decision.action in CapabilityGate.REVIEW_REQUIRED,
            source="gguf_local_adapter",
        )
        llm_decision.safe = self.gate.can_execute(llm_decision, reviewed=reviewed)
        return llm_decision


def infer_os_action(prompt: Optional[str], system_state: Optional[Dict[str, Any]] = None) -> InferResult:
    runtime = LocalOSInferenceRuntime()
    return runtime.infer(prompt, system_state)


def infer_llm_action(prompt: Optional[str], system_state: Optional[Dict[str, Any]] = None, reviewed: bool = False) -> LLMActionDecision:
    adapter = LocalGGUFAdapter()
    return adapter.decide(prompt, system_state, reviewed=reviewed)

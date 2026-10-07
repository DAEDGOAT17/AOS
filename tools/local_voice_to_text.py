"""Deterministic local speech-to-intent and voice-to-text layer for AOS.

This is intentionally offline and deterministic. It behaves like a lightweight
ASR layer for the OS control plane: spoken phrases are normalized, matched
against a bounded grammar, mapped to safe shell actions, and otherwise returned
as plain transcribed text instead of invented commands.
"""

from __future__ import annotations

import argparse
import re
from dataclasses import dataclass
from typing import Dict, Iterable, List, Optional

DEFAULT_VOCABULARY: Dict[str, List[str]] = {
    "help": [
        "help",
        "help me",
        "show help",
        "what can you do",
        "open help",
        "please help",
        "show me the commands",
        "show the commands",
    ],
    "ls /": [
        "list files",
        "list the files",
        "show files",
        "show me the files",
        "list directory",
        "list the directory",
        "show directory",
        "display files",
    ],
    "sysinfo": [
        "status",
        "system status",
        "show system status",
        "show me the system status",
        "check system status",
        "check the system status",
        "give me system info",
        "show system info",
        "show me system info",
        "tell me about the state of the machine",
        "show me the state of the machine",
        "tell me about the machine state",
    ],
    "clear": [
        "clear",
        "clear screen",
        "clear the screen",
        "wipe the screen",
        "erase the screen",
    ],
    "reboot": [
        "reboot",
        "restart",
        "restart the system",
        "reboot the system",
        "please reboot",
    ],
    "stop": [
        "stop",
        "halt",
        "shutdown",
        "stop the system",
        "halt the system",
    ],
    "voice train help": [
        "train help",
        "train the help command",
        "teach help",
        "teach the help command",
    ],
    "voice train status": [
        "train status",
        "train the status command",
        "teach status",
        "teach the status command",
    ],
}


@dataclass
class SpeechIntent:
    raw: str
    normalized: str
    command: str
    mode: str
    confidence: float


class LocalIntentParser:
    """Local deterministic intent parser used as a tiny ASR-style front end."""

    def __init__(self, vocabulary: Optional[Dict[str, Iterable[str]]] = None) -> None:
        self.vocabulary = {key: [normalize_utterance(phrase) for phrase in values] for key, values in (vocabulary or DEFAULT_VOCABULARY).items()}

    def parse(self, utterance: Optional[str]) -> SpeechIntent:
        raw = utterance or ""
        normalized = normalize_utterance(raw)
        command = self._match_command(normalized)
        mode = "known" if command != normalized else "transcribe"
        confidence = 1.0 if mode == "known" else 0.15
        return SpeechIntent(
            raw=raw.strip(),
            normalized=normalized,
            command=command,
            mode=mode,
            confidence=confidence,
        )

    def _match_command(self, normalized: str) -> str:
        if not normalized:
            return ""

        for canonical, phrases in self.vocabulary.items():
            for phrase in phrases:
                if phrase == normalized:
                    return canonical
                if normalized.startswith(phrase + " ") or normalized.endswith(" " + phrase):
                    return canonical
                if normalized in phrase:
                    return canonical

        for canonical, phrases in self.vocabulary.items():
            utterance_tokens = set(normalized.split())
            for phrase in phrases:
                phrase_tokens = set(phrase.split())
                if phrase_tokens and utterance_tokens.issuperset(phrase_tokens):
                    return canonical

        return normalized


def normalize_utterance(value: Optional[str]) -> str:
    """Normalize a spoken phrase so matching is stable and deterministic."""
    if value is None:
        return ""
    text = value.lower()
    text = re.sub(r"[^a-z0-9\s]", " ", text)
    text = re.sub(r"\s+", " ", text).strip()
    return text


def local_voice_to_text(utterance: Optional[str], vocabulary: Optional[Dict[str, Iterable[str]]] = None) -> str:
    """Return a canonical command when a phrase matches a known local action."""
    parser = LocalIntentParser(vocabulary=vocabulary)
    return parser.parse(utterance).command


def infer_os_action(utterance: Optional[str], vocabulary: Optional[Dict[str, Iterable[str]]] = None) -> Dict[str, str]:
    """Emit a structured local action payload from a spoken phrase."""
    parser = LocalIntentParser(vocabulary=vocabulary)
    intent = parser.parse(utterance)
    return {
        "raw": intent.raw,
        "normalized": intent.normalized,
        "command": intent.command,
        "mode": intent.mode,
        "confidence": str(intent.confidence),
    }


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Deterministic local speech-to-intent for the AOS kernel")
    parser.add_argument("phrase", nargs="*", help="Spoken phrase to interpret")
    args = parser.parse_args()

    incoming = " ".join(args.phrase)
    intent = LocalIntentParser().parse(incoming)
    print(intent.command)

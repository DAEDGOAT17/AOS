#!/usr/bin/env python3
"""Validate generated AOS docs artifacts and the primary newcomer navigation."""

from __future__ import annotations

from html.parser import HTMLParser
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
DOCS = ROOT / "docs" / "blog_docs"


class LinkParser(HTMLParser):
    def __init__(self) -> None:
        super().__init__()
        self.links: list[str] = []
        self.ids: set[str] = set()

    def handle_starttag(self, tag: str, attrs: list[tuple[str, str | None]]) -> None:
        values = dict(attrs)
        if tag == "a" and values.get("href"):
            self.links.append(str(values["href"]))
        if values.get("id"):
            self.ids.add(str(values["id"]))


def check_relative_link(source: Path, link: str) -> str | None:
    if link.startswith(("https://", "http://", "mailto:", "#", "javascript:")):
        return None
    target, _, anchor = link.partition("#")
    resolved = (source.parent / target).resolve() if target else source.resolve()
    if not resolved.exists():
        return f"{source.relative_to(ROOT)}: missing target {link}"
    if anchor and resolved.suffix.lower() == ".html":
        parser = LinkParser()
        parser.feed(resolved.read_text(encoding="utf-8", errors="replace"))
        if anchor not in parser.ids:
            return f"{source.relative_to(ROOT)}: missing anchor #{anchor} in {resolved.relative_to(ROOT)}"
    return None


def main() -> int:
    failures: list[str] = []
    portal = DOCS / "index.html"
    glossary = DOCS / "FUNCTION_GLOSSARY.html"
    search_index = DOCS / "search-index.js"
    guide = DOCS / "START_HERE.md"
    for required in (portal, glossary, search_index, guide):
        if not required.is_file():
            failures.append(f"missing required docs artifact: {required.relative_to(ROOT)}")

    for source in (portal, glossary):
        parser = LinkParser()
        parser.feed(source.read_text(encoding="utf-8", errors="replace"))
        for link in parser.links:
            problem = check_relative_link(source, link)
            if problem:
                failures.append(problem)

    markdown_link = re.compile(r"!?\[[^\]]*\]\(([^)]+)\)")
    for source in (guide, DOCS / "README.md"):
        for link in markdown_link.findall(source.read_text(encoding="utf-8", errors="replace")):
            problem = check_relative_link(source, link)
            if problem:
                failures.append(problem)

    generated = search_index.read_text(encoding="utf-8") if search_index.exists() else ""
    if "window.AOS_DOC_SEARCH = [" not in generated:
        failures.append("search-index.js does not define window.AOS_DOC_SEARCH")
    if "fat32_open" not in generated or "agent_task" not in generated:
        failures.append("search index is missing representative AOS API symbols")

    glossary_text = (DOCS / "FUNCTION_GLOSSARY.md").read_text(encoding="utf-8") if (DOCS / "FUNCTION_GLOSSARY.md").exists() else ""
    api_count = len(re.findall(r"^### <a id=\"fn-", glossary_text, re.M))
    definition_count = len(re.findall(r"^#### <a id=\"impl-", glossary_text, re.M))
    if api_count < 100 or definition_count < api_count:
        failures.append(f"glossary coverage looks incomplete: {api_count} public, {definition_count} definitions")

    if failures:
        print("Documentation validation failed:", file=sys.stderr)
        for failure in failures:
            print(f"- {failure}", file=sys.stderr)
        return 1
    print(f"Documentation validation passed: {api_count} public APIs, {definition_count} definitions.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
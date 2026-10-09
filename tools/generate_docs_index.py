#!/usr/bin/env python3
"""Generate the AOS public C API glossary and static documentation search data."""

from __future__ import annotations

from html import escape
from html.parser import HTMLParser
import json
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
INCLUDE = ROOT / "include"
SOURCE = ROOT / "src"
DOCS = ROOT / "docs" / "blog_docs"
API_OUTPUT = DOCS / "FUNCTION_GLOSSARY.md"
API_HTML_OUTPUT = DOCS / "FUNCTION_GLOSSARY.html"
SEARCH_OUTPUT = DOCS / "search-index.js"
GITHUB_SOURCE_BASE = "https://github.com/DAEDGOAT17/AOS/blob/pankaj/"

MODULES = {
    "acpi": ("Firmware and boot", "ACPI table discovery and power/platform metadata."),
    "agent": ("Agent runtime", "Task state, persistent context, and bounded agent lifecycle."),
    "ahci": ("Storage drivers", "AHCI controller discovery and SATA block I/O."),
    "ata": ("Storage drivers", "ATA device setup, partitions, and sector access."),
    "gpt": ("Storage drivers", "GPT partition-table parsing."),
    "fat32": ("Filesystem", "FAT32 mount, directory, path, and file operations."),
    "gdt": ("CPU and interrupts", "Global descriptor table setup."),
    "idt": ("CPU and interrupts", "Interrupt descriptor table and exception handling."),
    "io": ("CPU and interrupts", "x86 port I/O primitives."),
    "pmm": ("Memory management", "Physical memory map and page-frame allocation."),
    "vmm": ("Memory management", "Virtual address mapping and page-table operations."),
    "kmalloc": ("Memory management", "Kernel heap allocation."),
    "task": ("Scheduling and timers", "Task creation, scheduling, and task state."),
    "timer": ("Scheduling and timers", "PIT timer, ticks, and timekeeping."),
    "shell": ("Shell and language runtime", "Interactive shell input, Lisp apps, and runtime commands."),
    "infer": ("Shell and language runtime", "Local intent classification and typed action routing."),
    "lisp": ("Shell and language runtime", "Validation and execution of bounded AOS Lisp."),
    "jit_engine": ("Shell and language runtime", "Restricted runtime native-stub entry point."),
    "jit_runtime": ("Shell and language runtime", "Validated JIT stub construction and execution."),
    "ksym": ("Shell and language runtime", "Kernel symbol export and lookup."),
    "self_evolve": ("Shell and language runtime", "Runtime patch transport and controlled extension."),
    "screen": ("Display and input", "Framebuffer/VGA text output and screen capture."),
    "font8x8": ("Display and input", "Built-in 8x8 bitmap font data."),
    "pci": ("Device discovery", "PCI configuration-space access and device enumeration."),
    "hda_audio": ("Device discovery", "High Definition Audio device interface."),
    "net": ("Networking", "Network-device registration and common interface."),
    "net_stack": ("Networking", "AI HTTP client, agent transport, and ICMP ping."),
    "e1000": ("Networking", "Intel E1000 network-device interface."),
    "rtl8169": ("Networking", "Realtek RTL8169 network-device interface."),
    "serial": ("Display and input", "Serial console input/output."),
    "voice": ("Voice", "Voice command and local recognition interface."),
    "whisper": ("Voice", "Whisper model parsing, loading, and inference."),
    "math": ("Kernel libraries", "Fixed-point and numeric helpers."),
    "stdlib": ("Kernel libraries", "Freestanding standard-library helpers."),
    "string": ("Kernel libraries", "Freestanding string and memory functions."),
    "e1000_mock": ("Networking", "Mock E1000 device for test/runtime environments."),
    "trap_recovery": ("CPU and interrupts", "Trap and fault recovery support."),
}

FUNCTION_DESCRIPTIONS = {
    "ask": "Start an AI-directed task; AOS validates each proposed tool and returns observed output to the model.",
    "agent_task": "Start a bounded agent task and persist its lifecycle state.",
    "agent_ctx_get": "Read a named value from persistent agent context.",
    "agent_ctx_set": "Write a named value to persistent agent context.",
    "aos_lisp_validate": "Check whether source belongs to the bounded AOS Lisp grammar.",
    "aos_lisp_execute": "Evaluate a validated AOS Lisp expression in the kernel runtime.",
    "fat32_open": "Open a path from the FAT32 root/current directory for read, replace, or append.",
    "fat32_read": "Read bytes from an open FAT32 file handle.",
    "fat32_write": "Write bytes to an open FAT32 file handle.",
    "fat32_list_dir": "Enumerate entries in a FAT32 directory using a callback.",
    "shell_install_lisp_command": "Validate and persist a named Lisp command in the AOS app directory.",
    "shell_queue_lisp_app": "Queue a saved Lisp app for execution by the shell task.",
    "shell_queue_lisp_expression": "Queue a validated Lisp expression for shell-task execution.",
    "jit_compile_and_load": "Accept only the JIT's narrow driver entry contract and execute its supported native stub.",
    "ollama_agent_request": "Start an agent request through the configured Ollama-compatible endpoint.",
    "ollama_feedback_request": "Send captured OS tool output back to the model for the next bounded decision.",
    "screen_start_capture": "Begin collecting text emitted by shell commands for agent observation.",
    "screen_stop_capture": "Finish screen capture for the current command output.",
    "screen_get_capture": "Return the text captured from the most recent command.",
}


def clean_c_source(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", lambda match: "\n" * match.group(0).count("\n"), text, flags=re.S)
    text = re.sub(r"//[^\n]*", "", text)
    cleaned: list[str] = []
    skipping_macro = False
    for line in text.splitlines(keepends=True):
        if skipping_macro:
            skipping_macro = line.rstrip().endswith("\\")
            cleaned.append("\n" if line.endswith("\n") else "")
            continue
        if line.lstrip().startswith("#"):
            skipping_macro = line.rstrip().endswith("\\")
            cleaned.append("\n" if line.endswith("\n") else "")
        else:
            cleaned.append(line)
    return "".join(cleaned)


def parse_prototype(statement: str) -> tuple[str, str] | None:
    declaration = " ".join(statement.split()).strip()
    if not declaration or "(" not in declaration or "typedef" in declaration or "(*" in declaration:
        return None
    if declaration.startswith(("struct ", "union ", "enum ")) or "=" in declaration:
        return None
    open_paren = declaration.find("(")
    before = declaration[:open_paren]
    name_match = re.search(r"([A-Za-z_]\w*)\s*$", before)
    if not name_match:
        return None
    name = name_match.group(1)
    return_type = before[:name_match.start()].strip()
    if not return_type or return_type in {"if", "while", "switch", "return"}:
        return None
    return name, declaration


def header_functions(path: Path) -> list[dict[str, str | int]]:
    original = path.read_text(encoding="utf-8", errors="replace")
    source = clean_c_source(original)
    functions: list[dict[str, str | int]] = []
    statement_start = 0
    braces = 0
    parentheses = 0

    for offset, character in enumerate(source):
        if character == "{":
            braces += 1
        elif character == "}":
            braces = max(0, braces - 1)
        elif character == "(":
            parentheses += 1
        elif character == ")":
            parentheses = max(0, parentheses - 1)
        elif character == ";" and braces == 0 and parentheses == 0:
            statement = source[statement_start:offset + 1]
            parsed = parse_prototype(statement)
            if parsed:
                name, signature = parsed
                declaration_offset = statement_start + len(statement) - len(statement.lstrip())
                line = source.count("\n", 0, declaration_offset) + 1
                functions.append({"name": name, "signature": signature, "line": line})
            statement_start = offset + 1

    return functions


def module_for(header: Path) -> tuple[str, str]:
    stem = header.stem
    return MODULES.get(stem, ("Kernel API", f"Public declarations from the {stem} interface."))


def function_description(name: str, module_summary: str) -> str:
    if name in FUNCTION_DESCRIPTIONS:
        return FUNCTION_DESCRIPTIONS[name]
    stem = name.split("_", 1)[0]
    if name.startswith("fat32_"):
        return f"{module_summary} Handles the {name.removeprefix('fat32_').replace('_', ' ')} operation."
    if name.startswith("ata_"):
        return f"{module_summary} Handles the {name.removeprefix('ata_').replace('_', ' ')} operation."
    if name.startswith("pmm_"):
        return f"{module_summary} Handles the {name.removeprefix('pmm_').replace('_', ' ')} operation."
    if name.startswith("vmm_"):
        return f"{module_summary} Handles the {name.removeprefix('vmm_').replace('_', ' ')} operation."
    if name.startswith("task_"):
        return f"{module_summary} Handles the {name.removeprefix('task_').replace('_', ' ')} operation."
    return f"{module_summary} See the declaration and implementation for the exact contract."


def collect_public_api() -> list[dict[str, str | int]]:
    entries: dict[str, dict[str, str | int]] = {}
    for header in sorted(INCLUDE.rglob("*.h")):
        relative = header.relative_to(INCLUDE).as_posix()
        if relative.startswith("lwip_port/") or header.name == "font8x8.h":
            continue
        module, module_summary = module_for(header)
        for function in header_functions(header):
            name = str(function["name"])
            entry = {
                **function,
                "header": f"include/{relative}",
                "module": module,
                "description": function_description(name, module_summary),
                "anchor": "fn-" + re.sub(r"[^a-z0-9-]", "-", name.lower()),
            }
            previous = entries.get(name)
            if previous and previous["signature"] != entry["signature"]:
                raise RuntimeError(f"Conflicting public declarations for {name}")
            entries[name] = entry
    if not entries:
        raise RuntimeError("No public function declarations found in include headers")
    return sorted(entries.values(), key=lambda entry: (str(entry["module"]), str(entry["name"])))


def source_function_definitions(public_names: set[str]) -> list[dict[str, str | int]]:
    definition_pattern = re.compile(
        r"(?m)^[ \t]*(?:(?:static|inline|__inline__|_Noreturn)\s+)*"
        r"(?P<return>(?:(?:struct|union|enum)\s+)?[A-Za-z_]\w*\s*\**)[ \t\n]+"
        r"(?P<name>[A-Za-z_]\w*)\s*\((?P<params>[^{};]*)\)\s*\{"
    )
    literal_pattern = re.compile(r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'')
    definitions: list[dict[str, str | int]] = []
    for path in sorted(SOURCE.rglob("*.c")):
        relative_source = path.relative_to(ROOT).as_posix()
        if relative_source.startswith("src/net/lwip/"):
            continue
        original = path.read_text(encoding="utf-8", errors="replace")
        source = clean_c_source(original)
        masked = literal_pattern.sub(lambda match: "".join("\n" if char == "\n" else " " for char in match.group(0)), source)
        module, description = module_for(path.with_suffix(".h"))
        for match in definition_pattern.finditer(masked):
            name = match.group("name")
            signature = " ".join(source[match.start():match.end() - 1].split())
            line = source.count("\n", 0, match.start()) + 1
            anchor_slug = re.sub(r"[^a-z0-9-]", "-", f"{relative_source}-{name}-{line}".lower())
            visibility = "Public implementation" if name in public_names else "Internal implementation"
            definitions.append({
                "name": name,
                "signature": signature,
                "line": line,
                "source": relative_source,
                "module": module,
                "description": f"{visibility} in `{relative_source}`. {description}",
                "anchor": "impl-" + anchor_slug,
            })
    if not definitions:
        raise RuntimeError("No AOS C function definitions found under src/")
    return definitions


def markdown_api(entries: list[dict[str, str | int]], definitions: list[dict[str, str | int]]) -> str:
    lines = [
        "# AOS Function Glossary",
        "",
        "> Generated by `python3 tools/generate_docs_index.py` from AOS headers and C source.",
        "> Includes AOS public header declarations and all AOS-owned C function definitions. Vendored `src/net/lwip/` is excluded.",
        "",
        f"**Coverage:** {len(entries)} unique public declarations and {len(definitions)} AOS C definitions.",
        "",
        "Use the [HTML glossary](FUNCTION_GLOSSARY.html) for browser search. Each entry links to its header or implementation line.",
        "",
        "## Public API declarations",
        "",
    ]
    modules: dict[str, list[dict[str, str | int]]] = {}
    for entry in entries:
        modules.setdefault(str(entry["module"]), []).append(entry)
    for module, functions in modules.items():
        lines.extend([f"## {module}", ""])
        for entry in functions:
            source_path = GITHUB_SOURCE_BASE + str(entry["header"])
            lines.extend([
                f"### <a id=\"{entry['anchor']}\"></a>`{entry['name']}`",
                "",
                f"`{entry['signature']}`",
                "",
                str(entry["description"]),
                "",
                f"Declaration: [{entry['header']}:{entry['line']}]({source_path}#L{entry['line']})",
                "",
            ])
    lines.extend(["## All AOS C function definitions", ""])
    source_groups: dict[str, list[dict[str, str | int]]] = {}
    for entry in definitions:
        source_groups.setdefault(str(entry["source"]), []).append(entry)
    for source, functions in source_groups.items():
        lines.extend([f"### `{source}`", ""])
        for entry in functions:
            source_path = GITHUB_SOURCE_BASE + source
            lines.extend([
                f"#### <a id=\"{entry['anchor']}\"></a>`{entry['name']}`",
                "",
                f"`{entry['signature']}`",
                "",
                str(entry["description"]),
                "",
                f"Definition: [{source}:{entry['line']}]({source_path}#L{entry['line']})",
                "",
            ])
    return "\n".join(lines)


def html_api(entries: list[dict[str, str | int]], definitions: list[dict[str, str | int]]) -> str:
    sections: list[str] = []
    groups: list[tuple[str, list[dict[str, str | int]], bool]] = []
    public_groups: dict[str, list[dict[str, str | int]]] = {}
    implementation_groups: dict[str, list[dict[str, str | int]]] = {}
    for entry in entries:
        public_groups.setdefault(str(entry["module"]), []).append(entry)
    for entry in definitions:
        implementation_groups.setdefault(str(entry["source"]), []).append(entry)
    groups.extend((module, items, True) for module, items in public_groups.items())
    groups.extend((source, items, False) for source, items in implementation_groups.items())
    for heading, functions, public in groups:
        sections.append(f'<section><h2>{escape(heading)}</h2><div class="entries">')
        for entry in functions:
            file_key = str(entry["header"] if public else entry["source"])
            target = GITHUB_SOURCE_BASE + file_key + f"#L{entry['line']}"
            kind = "Public API" if public else "Implementation"
            sections.append(
                f'<article class="entry" id="{escape(str(entry["anchor"]))}" '
                f'data-search="{escape(str(entry["name"] + " " + entry["signature"] + " " + entry["description"] + " " + file_key), quote=True)}">'
                f'<div class="entry-head"><code>{escape(str(entry["name"]))}</code><span>{kind}</span></div>'
                f'<pre><code>{escape(str(entry["signature"]))}</code></pre>'
                f'<p>{escape(str(entry["description"]))}</p>'
                f'<a href="{escape(target, quote=True)}" target="_blank" rel="noopener noreferrer">View highlighted source</a>'
                f'<small>{escape(file_key)}:{entry["line"]}</small></article>'
            )
        sections.append("</div></section>")
    return """<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1">
<title>AOS Function Glossary</title>
<style>
:root{color-scheme:light;--ink:#17272c;--muted:#53666b;--paper:#f4f7f5;--line:#c9d4d0;--accent:#087f6b;--soft:#e2efea}
*{box-sizing:border-box}body{margin:0;background:var(--paper);color:var(--ink);font:16px/1.55 system-ui,sans-serif}
header{padding:2.5rem max(1.25rem,calc((100vw - 1100px)/2));background:#102d32;color:#f7fffc}
header p{max-width:70ch;color:#cee0dc}h1,h2,h3,p{margin-top:0}main{max-width:1100px;margin:auto;padding:1.5rem}
.search{position:sticky;top:0;background:var(--paper);padding:1rem 0;z-index:2;border-bottom:1px solid var(--line)}
input{width:100%;padding:.9rem 1rem;border:1px solid #8da49c;border-radius:4px;font:inherit;background:white;color:var(--ink)}
.meta{color:var(--muted);font-size:.9rem;margin:.5rem 0}.entry{padding:1rem 0;border-bottom:1px solid var(--line);scroll-margin-top:8rem}
.entry-head{display:flex;justify-content:space-between;gap:1rem}.entry-head code{font-size:1.1rem;color:#005e50}.entry-head span{color:var(--muted);font-size:.85rem}
pre{overflow:auto;background:#e9efec;padding:.75rem;border-left:3px solid var(--accent)}.entry p{margin-bottom:.4rem;color:#31464a}
a{color:#006b59}section{margin:2rem 0}section h2{padding-bottom:.4rem;border-bottom:2px solid var(--accent)}
.hidden{display:none!important}.empty{padding:1.5rem;background:white;border:1px solid var(--line)}
@media(max-width:600px){main{padding:1rem}.entry-head{display:block}.entry-head span{display:block}}
</style>
</head>
<body><header><h1>AOS Function Glossary</h1><p>Search public header APIs and every AOS-owned C function definition. Private helpers are included; vendored lwIP implementation is excluded.</p></header>
<main><div class="search"><label for="q">Search by function, module, or concept</label><input id="q" type="search" autocomplete="off" placeholder="e.g. fat32_open, timer, agent memory"><p class="meta" id="count" aria-live="polite"></p></div>
<p class="meta">Generated from project headers and source by <code>tools/generate_docs_index.py</code>.</p>
""" + "\n".join(sections) + """
<p class="empty hidden" id="empty">No matching functions. Try a shorter term or module name.</p></main>
<script>
const q=document.querySelector('#q'), entries=[...document.querySelectorAll('.entry')], count=document.querySelector('#count'), empty=document.querySelector('#empty');
function filter(){const terms=q.value.toLowerCase().trim().split(/\\s+/).filter(Boolean);let visible=0;for(const entry of entries){const hay=(entry.dataset.search+' '+entry.textContent).toLowerCase();const show=terms.every(term=>hay.includes(term));entry.classList.toggle('hidden',!show);if(show)visible++;}for(const section of document.querySelectorAll('section'))section.classList.toggle('hidden',!section.querySelector('.entry:not(.hidden)'));count.textContent=visible+' function'+(visible===1?'':'s')+' of '+entries.length;empty.classList.toggle('hidden',visible!==0)}
q.addEventListener('input',filter);filter();
</script></body></html>"""


class VisibleHTML(HTMLParser):
    def __init__(self) -> None:
        super().__init__()
        self.parts: list[str] = []
        self.hidden = 0
        self.title = False
        self.title_text: list[str] = []

    def handle_starttag(self, tag: str, attrs: list[tuple[str, str | None]]) -> None:
        if tag in {"script", "style", "noscript"}:
            self.hidden += 1
        if tag == "title":
            self.title = True

    def handle_endtag(self, tag: str) -> None:
        if tag in {"script", "style", "noscript"} and self.hidden:
            self.hidden -= 1
        if tag == "title":
            self.title = False

    def handle_data(self, data: str) -> None:
        if self.title:
            self.title_text.append(data)
        if not self.hidden:
            self.parts.append(data)


def plain_text(path: Path) -> tuple[str, str]:
    raw = path.read_text(encoding="utf-8", errors="replace")
    if path.suffix.lower() == ".html":
        parser = VisibleHTML()
        parser.feed(raw)
        title = " ".join(" ".join(parser.title_text).split())
        text = " ".join(" ".join(parser.parts).split())
        return title or path.stem.replace("_", " ").title(), text
    heading = re.search(r"(?m)^#\s+(.+)$", raw)
    title = heading.group(1).strip() if heading else path.stem.replace("_", " ").title()
    text = re.sub(r"```.*?```", " ", raw, flags=re.S)
    text = re.sub(r"!?(\[([^\]]*)\])\([^)]*\)", r"\2", text)
    text = re.sub(r"[`*_>#|]", " ", text)
    return title, " ".join(text.split())


def search_entries(api: list[dict[str, str | int]],
                   definitions: list[dict[str, str | int]]) -> list[dict[str, str]]:
    entries: list[dict[str, str]] = []
    for path in sorted(DOCS.rglob("*")):
        if path.suffix.lower() not in {".md", ".html"} or path.name in {
            "index.html", "SUMMARY.html", "FUNCTION_GLOSSARY.md", "FUNCTION_GLOSSARY.html"
        }:
            continue
        title, text = plain_text(path)
        relative = path.relative_to(DOCS).as_posix()
        entries.append({
            "title": title,
            "kind": "guide",
            "href": relative,
            "text": text[:12000],
        })
    for function in api:
        entries.append({
            "title": str(function["name"]),
            "kind": "function",
            "href": f"FUNCTION_GLOSSARY.html#{function['anchor']}",
            "text": f"{function['signature']} {function['description']} {function['header']}",
        })
    for function in definitions:
        entries.append({
            "title": str(function["name"]),
            "kind": "implementation",
            "href": f"FUNCTION_GLOSSARY.html#{function['anchor']}",
            "text": f"{function['signature']} {function['description']} {function['source']}",
        })
    return entries


def main() -> int:
    api = collect_public_api()
    definitions = source_function_definitions({str(entry["name"]) for entry in api})
    API_OUTPUT.write_text(markdown_api(api, definitions), encoding="utf-8")
    API_HTML_OUTPUT.write_text(html_api(api, definitions), encoding="utf-8")
    entries = search_entries(api, definitions)
    payload = json.dumps(entries, ensure_ascii=True, separators=(",", ":"))
    SEARCH_OUTPUT.write_text(
        "// Generated by tools/generate_docs_index.py; do not edit by hand.\n"
        f"window.AOS_DOC_SEARCH = {payload};\n",
        encoding="utf-8",
    )
    print(f"Generated {API_OUTPUT.relative_to(ROOT)} with {len(api)} public functions")
    print(f"Included {len(definitions)} AOS-owned C function definitions")
    print(f"Generated {API_HTML_OUTPUT.relative_to(ROOT)}")
    print(f"Generated {SEARCH_OUTPUT.relative_to(ROOT)} with {len(entries)} searchable entries")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
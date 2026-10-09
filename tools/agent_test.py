#!/usr/bin/env python3
"""Check the local Ollama-to-AOS agent workflow over TCP."""

from __future__ import annotations

import argparse
import json
import re
import socket
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import aos_command_bridge as bridge


def check_ollama(model: str, base_url: str) -> bool:
	tags_url = base_url.rstrip("/") + "/api/tags"
	try:
		with urllib.request.urlopen(tags_url, timeout=3) as response:
			data = json.loads(response.read().decode("utf-8"))
	except (OSError, urllib.error.URLError, json.JSONDecodeError) as exc:
		print(f"FAIL Ollama is unavailable at {tags_url}: {exc}")
		print("Start Ollama on this computer, then rerun the test.")
		return False

	models = {item.get("name") for item in data.get("models", [])}
	if model not in models:
		print(f"FAIL model {model!r} is not downloaded in Ollama.")
		print("Available models: " + (", ".join(sorted(models)) or "none"))
		print(f"Download it with: ollama pull {model}")
		return False

	print(f"PASS Ollama is running and model {model!r} is available.")
	return True


def check_aos_listener(host: str, port: int, timeout: float) -> bool:
	try:
		with socket.create_connection((host, port), timeout=timeout) as sock:
			sock.settimeout(timeout)
			banner = sock.recv(512).decode("utf-8", errors="replace")
	except OSError as exc:
		print(f"FAIL cannot connect to AOS at {host}:{port}: {exc}")
		print("Check that AOS is booted, has a 192.168.77.x address, and its TCP listener is active on port 9001.")
		print("On the LOQ, verify the peer with: ip neigh show dev enp7s0")
		return False

	if "AOS remote command listener ready" not in banner:
		print(f"FAIL {host}:{port} answered, but did not identify as the AOS command listener: {banner!r}")
		return False

	print(f"PASS AOS command listener is reachable at {host}:{port}.")
	return True


def send_test_command(host: str, port: int, command: str, expected: str, timeout: float) -> str:
	response = bytearray()
	deadline = time.monotonic() + timeout
	with socket.create_connection((host, port), timeout=timeout) as sock:
		sock.sendall((command + "\n").encode("utf-8"))
		sock.settimeout(min(timeout, 0.5))
		while len(response) < 8192 and time.monotonic() < deadline:
			try:
				chunk = sock.recv(min(2048, 8192 - len(response)))
			except socket.timeout:
				continue
			if not chunk:
				break
			response.extend(chunk)
			if expected.encode("utf-8") in response:
				break
	return response.decode("utf-8", errors="replace")


RUNTIME_PROGRAMS = {
	"pwd": ("pwd:print_current_directory", "pwd", "/"),
	"uptime": ("uptime:show_uptime", "uptime", "Uptime:"),
	"diskinfo": ("diskinfo:show_disk_info", "diskinfo", "Storage:"),
	"agentreport": ("agentreport:show_agent_report", "agentreport", "Saved task:"),
}


def inject_runtime_command(host: str, jit_port: int, command_port: int, timeout: float, program: str) -> int:
	if program not in RUNTIME_PROGRAMS:
		print(f"FAIL unsupported runtime program: {program}")
		return 1
	source, command, expected = RUNTIME_PROGRAMS[program]
	payload = (
		f"---BEGIN_DRIVER---\n"
		f"int driver_init(void) {{\n"
		f'    shell_install_runtime_command("{source}");\n'
		f"    return 0;\n"
		f"}}\n"
		f"---END_DRIVER---\n"
	).encode("ascii")
	response = bytearray()
	deadline = time.monotonic() + timeout

	try:
		with socket.create_connection((host, jit_port), timeout=timeout) as sock:
			sock.settimeout(0.25)
			while b"waiting for driver payload" not in response and time.monotonic() < deadline:
				try:
					chunk = sock.recv(1024)
				except socket.timeout:
					continue
				if not chunk:
					break
				response.extend(chunk)

			if b"waiting for driver payload" not in response:
				print(f"FAIL AOS JIT listener did not send its payload prompt at {host}:{jit_port}.")
				return 1

			sock.sendall(payload)
			while b"---RESULT: SUCCESS---" not in response and b"---RESULT: COMPILE_FAILED---" not in response and time.monotonic() < deadline:
				try:
					chunk = sock.recv(1024)
				except socket.timeout:
					continue
				if not chunk:
					break
				response.extend(chunk)
	except OSError as exc:
		print(f"FAIL runtime injection could not connect to AOS at {host}:{jit_port}: {exc}")
		print("Check that AOS is booted and its self-evolve listener is active on port 9000.")
		return 1

	print(response.decode("utf-8", errors="replace").rstrip())
	if b"---RESULT: SUCCESS---" not in response:
		print("FAIL AOS did not accept and execute the runtime payload.")
		return 1

	try:
		with socket.create_connection((host, command_port), timeout=timeout) as sock:
			sock.settimeout(timeout)
			sock.recv(512)
			sock.sendall((command + "\n").encode("ascii"))
			command_output = bytearray()
			deadline = time.monotonic() + timeout
			while len(command_output) < 2048 and time.monotonic() < deadline:
				try:
					chunk = sock.recv(1024)
				except socket.timeout:
					if command_output:
						break
					continue
				if not chunk:
					break
				command_output.extend(chunk)
		rendered = command_output.decode("utf-8", errors="replace")
		if command == "pwd":
			matched = any(line.strip().startswith("/") for line in rendered.splitlines())
		else:
			matched = expected in rendered
		if not matched:
			print(f"FAIL injected {command} did not return its expected output:")
			print(rendered.rstrip())
			return 1
		print(f"PASS injected {command} returned the expected output.")
	except OSError as exc:
		print(f"FAIL could not verify injected pwd over the AOS shell port: {exc}")
		return 1

	print("PASS runtime command installed. The installer reports whether the filesystem survives reboot.")
	return 0


def inject_lisp_command(host: str, jit_port: int, command_port: int, timeout: float,
					name: str, source: str, expected: str | None, argument: str = "",
					launch_app: bool = False) -> int:
	if not name or len(name) > 8 or not name.isascii() or not name[0].islower() or not name.isalpha():
		print("FAIL Lisp command names must start with a lowercase letter and fit FAT 8.3.")
		return 1
	if any(not (character.islower() or character.isdigit()) for character in name):
		print("FAIL Lisp command names may contain only lowercase letters and digits.")
		return 1
	definition = name + "\n" + source
	try:
		definition_bytes = definition.encode("ascii")
	except UnicodeEncodeError:
		print("FAIL Lisp programs must use ASCII source text.")
		return 1
	if len(definition_bytes) > 3010:
		print("FAIL Lisp definition exceeds the 3,000-byte runtime limit.")
		return 1
	quoted = definition.replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n")
	if len(quoted.encode("ascii")) >= 3584:
		print("FAIL escaped Lisp definition exceeds the JIT literal page.")
		return 1
	payload = (
		"---BEGIN_DRIVER---\n"
		"int driver_init(void) {\n"
		f'    shell_install_lisp_command("{quoted}");\n'
		"    return 0;\n"
		"}\n"
		"---END_DRIVER---\n"
	).encode("ascii")
	response = bytearray()
	deadline = time.monotonic() + timeout
	try:
		with socket.create_connection((host, jit_port), timeout=timeout) as sock:
			sock.settimeout(0.25)
			while b"waiting for driver payload" not in response and time.monotonic() < deadline:
				try:
					chunk = sock.recv(1024)
				except socket.timeout:
					continue
				if not chunk:
					break
				response.extend(chunk)
			if b"waiting for driver payload" not in response:
				print(f"FAIL no JIT payload prompt from {host}:{jit_port}.")
				return 1
			sock.sendall(payload)
			while b"---RESULT: SUCCESS---" not in response and b"---RESULT: COMPILE_FAILED---" not in response and time.monotonic() < deadline:
				try:
					chunk = sock.recv(1024)
				except socket.timeout:
					continue
				if not chunk:
					break
				response.extend(chunk)
	except OSError as exc:
		print(f"FAIL Lisp injection could not connect to AOS at {host}:{jit_port}: {exc}")
		return 1
	print(response.decode("utf-8", errors="replace").rstrip())
	if b"---RESULT: SUCCESS---" not in response:
		print("FAIL AOS rejected the Lisp command definition.")
		return 1
	if launch_app:
		try:
			queued = send_test_command(host, command_port, "runapp " + name, "queued", timeout)
		except OSError as exc:
			print(f"FAIL could not queue runtime Lisp app: {exc}")
			return 1
		if "queued" not in queued.lower():
			print("FAIL AOS did not queue the runtime Lisp app:")
			print(queued.rstrip())
			return 1
		print(f"PASS runtime Lisp app {name} installed and queued on AOS.")
		return 0
	if expected is None:
		print(f"PASS runtime Lisp app {name} installed.")
		return 0
	try:
		output = send_test_command(host, command_port, name + (" " + argument if argument else ""), expected, timeout)
	except OSError as exc:
		print(f"FAIL could not run installed Lisp command: {exc}")
		return 1
	if expected not in output:
		print("FAIL installed Lisp command did not produce expected output:")
		print(output.rstrip())
		return 1
	print(f"PASS runtime Lisp command {name} executed and returned expected output.")
	return 0


def generate_lisp_source(model: str, goal: str, endpoint: str) -> str:
	system_prompt = (
		"Generate one program in AOS Lisp. Return one S-expression only, no markdown, tags, comments, uppercase operators, or explanation. "
		"Valid examples: (begin (var running 1) (while (= running 1) (set! running 0)) (print running)); "
		"(begin (canvas 32 14) (let keycode (key) (begin (cell 2 2 \"O\") (draw) (wait 2)))). "
		"Allowed forms: begin, if, var name value, let name initial body, set! name value, while condition body, +, -, *, mod, =, !=, <, >, <=, >=, and, or, not, concat, print, canvas, cell, draw, key, wait. "
		"key takes zero or one integer fallback argument and returns an ASCII integer. wait takes one integer from 1 to 10. canvas takes width and height (max 60x20). cell takes x, y, and one-character string or ASCII integer. draw takes no args. "
		"Identifiers start with a letter and may contain letters, digits, underscores, or hyphens. Use var declarations inside begin to keep nesting shallow, then set! to update state. Do not use lambda, defun, exit, lists, or any other operation. "
		"Use a bounded while condition to end interactive apps. Keep one expression below 3,000 ASCII bytes and nesting at most 8."
	)
	request_goal = goal
	for attempt in range(3):
		payload = json.dumps({
			"model": model,
			"prompt": request_goal,
			"system": system_prompt,
			"stream": False,
			"options": {"temperature": 0.1, "num_predict": 3072},
		}).encode("utf-8")
		request = urllib.request.Request(endpoint, data=payload, headers={"Content-Type": "application/json"}, method="POST")
		with urllib.request.urlopen(request, timeout=120) as response:
			result = json.loads(response.read().decode("utf-8"))
		text = str(result.get("response", "")).strip()
		match = re.search(r"<AOS_LISP>\s*(.*?)\s*</AOS_LISP>", text, flags=re.S | re.I)
		if match:
			text = match.group(1).strip()
		else:
			text = re.sub(r"^```(?:lisp)?\s*|\s*```$", "", text, flags=re.I).strip()
			text = re.sub(r"^(?:EXPRESSION|LISP)\s*", "", text, flags=re.I)
		try:
			text = extract_lisp_expression(text)
			_, text = validate_lisp_source(text)
			return text
		except ValueError as exc:
			print(f"Rejected AI Lisp draft {attempt + 1}/3: {exc}; bytes={len(text.encode('utf-8', errors='replace'))}; preview={text[:120]!r}")
			if attempt == 2:
				raise
			request_goal = goal + "\nYour previous response was rejected: " + str(exc) + ". Return a corrected single expression using only the exact grammar."
			request_goal += "\nPrevious rejected draft:\n" + text + "\nRewrite that behavior using the valid examples and exact syntax."
	raise ValueError("Ollama did not produce a valid Lisp program")


def extract_lisp_expression(text: str) -> str:
	start = text.find("(")
	if start < 0:
		raise ValueError("response contains no Lisp expression")
	depth = 0
	in_string = False
	escaped = False
	for position in range(start, len(text)):
		value = text[position]
		if in_string:
			if escaped:
				escaped = False
			elif value == "\\":
				escaped = True
			elif value == '"':
				in_string = False
		elif value == '"':
			in_string = True
		elif value == "(":
			depth += 1
		elif value == ")":
			depth -= 1
			if depth == 0:
				return text[start:position + 1].strip()
	if depth > 0 and not in_string and not escaped:
		return (text[start:] + ")" * depth).strip()
	raise ValueError("response has an incomplete Lisp expression")


def validate_lisp_source(source: str) -> tuple[set[str], str]:
	if not source or len(source.encode("ascii")) > 8192:
		raise ValueError("source is empty, non-ASCII, or exceeds the generation buffer")
	tokens = re.findall(r'"(?:\\.|[^"\\])*"|[()]|[^\s()]+', source)
	position = 0
	node_count = 0
	operators = {
		"begin", "if", "let", "var", "set!", "while", "+", "-", "*", "mod", "=", "!=", "<", ">", "<=", ">=",
		"and", "or", "not", "concat", "print", "canvas", "cell", "draw", "key", "wait", "uptime", "diskinfo",
	}
	ident_pattern = re.compile(r"^[a-z][a-z0-9_-]*$", flags=re.I)
	aliases = {"progn": "begin", "defvar": "var", "setq": "set!", "equal": "=", "rem": "mod"}

	def canonicalize(node):
		if not isinstance(node, list):
			if isinstance(node, str) and not node.startswith('"') and not re.fullmatch(r"-?[0-9]+", node):
				return node.lower()
			return node
		name = aliases.get(node[0].lower(), node[0].lower())
		arguments = [canonicalize(item) for item in node[1:]]
		if name in (">=", "<=", "!=") and len(arguments) == 2:
			comparison = {">=": "<", "<=": ">", "!=": "="}[name]
			return ["not", [comparison, arguments[0], arguments[1]]]
		if name == "if" and len(arguments) == 2:
			arguments.append("false")
		if name in ("and", "or"):
			if not arguments:
				return "true" if name == "and" else "false"
			result = arguments[-1]
			for item in reversed(arguments[:-1]):
				result = [name, item, result]
			return result
		return [name] + arguments

	def parse(depth: int = 0):
		nonlocal position, node_count
		if depth > 8 or position >= len(tokens):
			raise ValueError("unexpected end or nesting exceeds 8")
		token = tokens[position]
		position += 1
		node_count += 1
		if node_count > 512:
			raise ValueError("expression exceeds 512 syntax nodes")
		if token == "(":
			items = []
			while position < len(tokens) and tokens[position] != ")":
				items.append(parse(depth + 1))
			if position >= len(tokens):
				raise ValueError("unclosed list")
			position += 1
			if not items or not isinstance(items[0], str) or items[0] not in operators:
				raise ValueError("unknown operator: " + repr(items[0] if items else None))
			name, count = items[0], len(items) - 1
			limits = {"begin": (1, 255), "if": (2, 3), "let": (3, 3), "var": (2, 2), "set!": (2, 2), "while": (2, 255),
				"+": (2, 4), "-": (1, 2), "*": (2, 4), "mod": (2, 2), "=": (2, 2), "!=": (2, 2), "<": (2, 2), ">": (2, 2), "<=": (2, 2), ">=": (2, 2),
				"and": (0, 4), "or": (0, 4), "not": (1, 1), "concat": (1, 4), "print": (1, 1),
				"canvas": (2, 2), "cell": (3, 3), "draw": (0, 0), "key": (0, 1), "wait": (1, 1), "uptime": (0, 0), "diskinfo": (0, 0)}
			minimum, maximum = limits[name]
			if not minimum <= count <= maximum:
				raise ValueError(f"{name} has the wrong number of arguments")
			if name in ("let", "var", "set!") and (not isinstance(items[1], str) or not ident_pattern.match(items[1]) or len(items[1]) > 15):
				raise ValueError(f"{name} requires a lowercase variable name")
			return items
		if token == ")":
			raise ValueError("unexpected close parenthesis")
		if token.startswith('"'):
			return token
		if re.fullmatch(r"-?[0-9]+", token):
			return token
		normalized = token.lower()
		if normalized in ("true", "false", "arg") or normalized in operators or ident_pattern.match(token):
			return normalized
		raise ValueError(f"unsupported token: {token[:40]!r}")

	tree = canonicalize(parse())
	if position != len(tokens):
		raise ValueError("trailing tokens after expression")
	def serialize(node) -> str:
		if isinstance(node, list):
			return "(" + " ".join(serialize(item) for item in node) + ")"
		return node
	compact_source = serialize(tree)
	if len(compact_source.encode("ascii")) > 3000:
		raise ValueError("canonical source exceeds 3,000 bytes")
	forms: set[str] = set()
	def collect(node):
		if isinstance(node, list):
			if node and isinstance(node[0], str):
				forms.add(node[0])
			for child in node:
				collect(child)
	collect(tree)
	return forms, compact_source


def read_remote_screen(host: str, port: int, timeout: float) -> int:
	response = bytearray()
	end_marker = b"---AOS_SCREEN_END---"
	try:
		with socket.create_connection((host, port), timeout=timeout) as sock:
			sock.sendall(b"screen_dump\n")
			sock.settimeout(0.5)
			deadline = time.monotonic() + timeout
			while end_marker not in response and len(response) < 16384 and time.monotonic() < deadline:
				try:
					chunk = sock.recv(min(4096, 16384 - len(response)))
				except socket.timeout:
					continue
				if not chunk:
					break
				response.extend(chunk)
	except OSError as exc:
		print(f"FAIL could not read AOS screen at {host}:{port}: {exc}")
		return 1

	begin_marker = b"---AOS_SCREEN_BEGIN---\n"
	if begin_marker not in response or end_marker not in response:
		print("FAIL AOS did not return a complete screen snapshot.")
		print(response.decode("utf-8", errors="replace"))
		return 1

	start = response.index(begin_marker) + len(begin_marker)
	end = response.index(end_marker, start)
	print(response[start:end].decode("utf-8", errors="replace"), end="")
	return 0


def run_test(host: str, port: int, model: str, ollama_url: str, timeout: float) -> int:
	base_url = ollama_url.rsplit("/api/", 1)[0]
	if not check_ollama(model, base_url):
		return 1
	if not check_aos_listener(host, port, timeout):
		return 1

	prompt = (
		"Print the exact text AOS_AGENT_TEST on AOS. "
		"Choose exactly the command echo AOS_AGENT_TEST and return it as <EXEC_CMD:echo AOS_AGENT_TEST>. "
		"Do not add explanation or any other command."
	)
	try:
		response = bridge.call_ollama(model, prompt, endpoint=ollama_url)
	except (OSError, urllib.error.URLError, json.JSONDecodeError) as exc:
		print(f"FAIL Ollama could not generate a command: {exc}")
		return 1

	command = bridge.extract_command(response)
	if not command or not bridge.is_safe_command(command):
		print(f"FAIL model did not return an accepted safe command: {response!r}")
		return 1
	print(f"PASS model selected safe command: {command}")

	try:
		result = send_test_command(host, port, command, "AOS_AGENT_TEST", timeout)
	except OSError as exc:
		print(f"FAIL command could not be sent to AOS: {exc}")
		return 1

	overflow_count = result.count("---RESULT: PAYLOAD_TOO_LARGE---")
	clean_output = "\n".join(
		line for line in result.splitlines()
		if line != "---RESULT: PAYLOAD_TOO_LARGE---"
	)
	print("AOS response:")
	print(clean_output.rstrip() or "(empty response)")
	if "AOS_AGENT_TEST" not in result:
		print("FAIL AOS did not return the expected echo output.")
		return 1

	if overflow_count:
		print(f"WARN AOS also emitted {overflow_count} output-buffer overflow marker(s).")
	print("PASS end-to-end agent workflow completed.")
	return 0


def main() -> int:
	parser = argparse.ArgumentParser(description="Test Ollama command planning and live AOS command execution")
	parser.add_argument("--host", default=bridge.DEFAULT_AOS_HOST, help="AOS IPv4 address")
	parser.add_argument("--port", type=int, default=bridge.DEFAULT_AOS_PORT, help="AOS command port (default: 9001)")
	parser.add_argument("--model", default=bridge.DEFAULT_MODEL, help="Downloaded Ollama model")
	parser.add_argument("--ollama-url", default=bridge.DEFAULT_OLLAMA_URL, help="Ollama generate API URL")
	parser.add_argument("--timeout", type=float, default=5.0, help="TCP connection and response timeout in seconds")
	parser.add_argument("--inject-runtime-pwd", action="store_true", help="install and verify pwd through the port 9000 JIT listener")
	parser.add_argument("--inject-runtime-command", choices=sorted(RUNTIME_PROGRAMS), help="install and verify an allowlisted runtime command through the JIT listener")
	parser.add_argument("--install-lisp-command", help="install a named persisted Lisp command (requires --lisp-source and --expect)")
	parser.add_argument("--generate-lisp-command", help="ask Ollama to generate and install a persisted Lisp app")
	parser.add_argument("--lisp-prompt", help="natural-language app request for --generate-lisp-command")
	parser.add_argument("--launch-after-install", action="store_true", help="queue the installed app to run on the AOS shell task")
	parser.add_argument("--lisp-source", help="one bounded Lisp expression for the installed command")
	parser.add_argument("--lisp-argument", default="", help="argument supplied when testing the installed Lisp command")
	parser.add_argument("--expect", help="expected command output substring")
	parser.add_argument("--jit-port", type=int, default=9000, help="AOS JIT listener port")
	parser.add_argument("--read-screen", action="store_true", help="read the latest 24 visible text rows from AOS over port 9001")
	args = parser.parse_args()
	if args.read_screen:
		return read_remote_screen(args.host, args.port, max(args.timeout, 10.0))
	if args.inject_runtime_pwd or args.inject_runtime_command:
		program = args.inject_runtime_command or "pwd"
		return inject_runtime_command(args.host, args.jit_port, args.port, max(args.timeout, 15.0), program)
	if args.install_lisp_command:
		if args.lisp_source is None or (args.expect is None and not args.launch_after_install):
			parser.error("--install-lisp-command requires --lisp-source and either --expect or --launch-after-install")
		return inject_lisp_command(args.host, args.jit_port, args.port, max(args.timeout, 15.0),
							   args.install_lisp_command, args.lisp_source, args.expect, args.lisp_argument,
							   args.launch_after_install)
	if args.generate_lisp_command:
		if not args.lisp_prompt:
			parser.error("--generate-lisp-command requires --lisp-prompt")
		try:
			source = generate_lisp_source(args.model, args.lisp_prompt, args.ollama_url)
		except (OSError, urllib.error.URLError, json.JSONDecodeError, ValueError, UnicodeEncodeError) as exc:
			print(f"FAIL Ollama could not produce a bounded AOS Lisp app: {exc}")
			return 1
		print("AI-generated AOS Lisp source:")
		print(source)
		return inject_lisp_command(args.host, args.jit_port, args.port, max(args.timeout, 15.0),
							   args.generate_lisp_command, source, None, "", args.launch_after_install)
	return run_test(args.host, args.port, args.model, args.ollama_url, args.timeout)


if __name__ == "__main__":
	sys.exit(main())

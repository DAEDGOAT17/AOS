#!/usr/bin/env python3
"""Check the local Ollama-to-AOS agent workflow over TCP."""

from __future__ import annotations

import argparse
import json
import socket
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import aos_command_bridge as bridge


RUNTIME_PROGRAMS = {
	"pwd": ("pwd:print_current_directory", ("/",)),
	"uptime": ("uptime:print_system_uptime", ("Uptime:",)),
	"diskinfo": ("diskinfo:print_storage_status", ("Storage:", "Sectors:", "FAT32 label:")),
	"agentreport": ("agentreport:print_saved_agent_report", ("Persisted agent report",)),
}


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


def run_runtime_command(host: str, command_port: int, command: str, timeout: float) -> int:
	_, expected_output = RUNTIME_PROGRAMS[command]
	try:
		with socket.create_connection((host, command_port), timeout=timeout) as sock:
			sock.settimeout(timeout)
			sock.recv(512)
			sock.sendall((command + "\n").encode("ascii"))
			command_output = bytearray()
			deadline = time.monotonic() + timeout
			while len(command_output) < 4096 and time.monotonic() < deadline:
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
		missing = [marker for marker in expected_output if marker not in rendered]
		if missing:
			print(f"FAIL {command} did not return expected output:")
			print(rendered.rstrip())
			return 1
		print(f"PASS {command} returned:")
		print(rendered.rstrip())
	except OSError as exc:
		print(f"FAIL could not run {command} over the AOS shell port: {exc}")
		return 1
	return 0


def install_runtime_command(host: str, jit_port: int, command_port: int,
							command: str, timeout: float) -> int:
	program, _ = RUNTIME_PROGRAMS[command]
	payload = (
		b"---BEGIN_DRIVER---\n"
		+ b"int driver_init(void) {\n"
		+ f'    shell_install_runtime_command("{program}");\n'.encode("ascii")
		+ b"    return 0;\n"
		+ b"}\n"
		+ b"---END_DRIVER---\n"
	)
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

	if run_runtime_command(host, command_port, command, timeout) != 0:
		return 1
	print(f"PASS runtime command {command} installed and executed.")
	return 0


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
	parser.add_argument("--install-runtime-command", choices=tuple(RUNTIME_PROGRAMS), help="install and verify an allowlisted persistent runtime command")
	parser.add_argument("--run-runtime-command", choices=tuple(RUNTIME_PROGRAMS), help="run and verify an installed runtime command without reinstalling it")
	parser.add_argument("--jit-port", type=int, default=9000, help="AOS JIT listener port")
	parser.add_argument("--read-screen", action="store_true", help="read the latest 24 visible text rows from AOS over port 9001")
	args = parser.parse_args()
	if args.read_screen:
		return read_remote_screen(args.host, args.port, max(args.timeout, 10.0))
	if args.run_runtime_command:
		return run_runtime_command(args.host, args.port, args.run_runtime_command, args.timeout)
	runtime_command = args.install_runtime_command or ("pwd" if args.inject_runtime_pwd else None)
	if runtime_command:
		return install_runtime_command(args.host, args.jit_port, args.port,
									   runtime_command, max(args.timeout, 15.0))
	return run_test(args.host, args.port, args.model, args.ollama_url, args.timeout)


if __name__ == "__main__":
	sys.exit(main())

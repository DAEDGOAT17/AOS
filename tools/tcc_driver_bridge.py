#!/usr/bin/env python3
import os
import subprocess
import sys
import tempfile


def usage() -> int:
    print("usage: tcc_driver_bridge.py <source.c>")
    return 1


def main() -> int:
    if len(sys.argv) != 2:
        return usage()

    src_path = sys.argv[1]
    if not os.path.exists(src_path):
        print(f"error: source file not found: {src_path}")
        return 2

    with open(src_path, "r", encoding="utf-8") as f:
        source_text = f.read()

    if "driver_init" not in source_text:
        print("error: generated driver does not define driver_init(void)")
        return 3

    temp_dir = tempfile.mkdtemp(prefix="aos_driver_")
    object_path = os.path.join(temp_dir, "driver.o")

    cmd = [
        "gcc",
        "-c",
        "-ffreestanding",
        "-fno-builtin",
        "-fno-stack-protector",
        "-nostdlib",
        "-I.",
        "-Iinclude",
        "-Isrc",
        src_path,
        "-o",
        object_path,
    ]

    result = subprocess.run(cmd, capture_output=True, text=True)

    if result.stdout:
        print(result.stdout)
    if result.stderr:
        print(result.stderr)

    if result.returncode != 0:
        print("compiler bridge: compile failed")
        return result.returncode

    print(f"compiler bridge: OK ({object_path})")
    return 0


if __name__ == "__main__":
    sys.exit(main())

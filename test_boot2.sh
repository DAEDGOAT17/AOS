#!/bin/bash
set -e
pkill -f "qemu-system-x86_64.*jarvis.iso" >/dev/null 2>&1 || true
pkill -f "qemu-system-x86_64.*disk.img" >/dev/null 2>&1 || true
rm -f serial2.log
qemu-system-x86_64 -m 2G -boot d -cdrom jarvis.iso -drive file=disk.img,format=raw -nographic < /dev/null > serial2.log 2>&1 &
PID=$!
sleep 15
if kill -0 "$PID" 2>/dev/null; then
    kill "$PID" || kill -9 "$PID"
    wait "$PID" || true
fi
sed -n '1,120p' serial2.log || true

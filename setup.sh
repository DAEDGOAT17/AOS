#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MODEL="${HARVIS_MODEL:-gemma3:4b}"
HOST_ADDRESS="192.168.77.1"
DHCP_RANGE_START="192.168.77.10"
DHCP_RANGE_END="192.168.77.30"
DHCP_LEASE_FILE="/var/lib/misc/dnsmasq-aos-link.leases"
DNSMASQ_PID_FILE="/run/dnsmasq-aos-link.pid"
RUNTIME_DIR="${XDG_RUNTIME_DIR:-${TMPDIR:-/tmp}}"
RELAY_PID_FILE="$RUNTIME_DIR/aos-ollama-lan-relay-$UID.pid"
RELAY_LOG_FILE="$RUNTIME_DIR/aos-ollama-lan-relay-$UID.log"
NETWORK_STATE_FILE="$RUNTIME_DIR/aos-link-network-$UID.state"

usage() {
    printf 'Usage: %s <loq-ethernet-interface> [aos-ip-address]\n' "$0"
    printf '       %s --stop-dhcp\n' "$0"
}

if [[ "${1:-}" == "--help" || $# -eq 0 ]]; then
    usage
    exit 0
fi

if [[ "${1:-}" == "--stop-dhcp" ]]; then
    if sudo test -f "$DNSMASQ_PID_FILE"; then
        dnsmasq_pid="$(sudo cat "$DNSMASQ_PID_FILE")"
        if [[ "$dnsmasq_pid" =~ ^[0-9]+$ ]]; then
            sudo kill "$dnsmasq_pid" 2>/dev/null || true
        fi
        sudo rm -f "$DNSMASQ_PID_FILE"
        printf 'Stopped the AOS-link DHCP server. The Ethernet address was left unchanged.\n'
    else
        printf 'No AOS-link DHCP PID file found.\n'
    fi
    if [[ -f "$NETWORK_STATE_FILE" ]]; then
        read -r managed_iface was_managed <"$NETWORK_STATE_FILE"
        if [[ "$was_managed" == "yes" ]] && command -v nmcli >/dev/null 2>&1; then
            sudo nmcli device set "$managed_iface" managed yes
            printf 'Restored NetworkManager control of %s.\n' "$managed_iface"
        fi
        rm -f "$NETWORK_STATE_FILE"
    fi
    exit 0
fi

if [[ $# -gt 2 ]]; then
    usage >&2
    exit 2
fi

IFACE="$1"
AOS_IP="${2:-}"

for command in ip sudo dnsmasq ollama python3 ps; do
    if ! command -v "$command" >/dev/null 2>&1; then
        printf 'Required command not found: %s\n' "$command" >&2
        exit 1
    fi
done

if ! ip link show dev "$IFACE" >/dev/null 2>&1; then
    printf 'Network interface not found: %s\n' "$IFACE" >&2
    ip -brief link
    exit 1
fi

if [[ ! -f "$ROOT_DIR/tools/aos_command_bridge.py" ]]; then
    printf 'Bridge script is missing: %s/tools/aos_command_bridge.py\n' "$ROOT_DIR" >&2
    exit 1
fi
if [[ ! -f "$ROOT_DIR/tools/ollama_lan_relay.py" ]]; then
    printf 'Ollama LAN relay script is missing: %s/tools/ollama_lan_relay.py\n' "$ROOT_DIR" >&2
    exit 1
fi

printf 'Selected Ethernet interface:\n'
ip -brief address show dev "$IFACE"
printf '\nThis setup assigns %s/24 to %s and starts DHCP for the directly connected AOS PC.\n' "$HOST_ADDRESS" "$IFACE"
printf 'Do not select the LOQ Wi-Fi interface.\n'
read -r -p 'Continue? [y/N] ' answer
if [[ ! "$answer" =~ ^[Yy]$ ]]; then
    printf 'Cancelled without changing network settings.\n'
    exit 0
fi

if ! python3 -c 'import urllib.request; urllib.request.urlopen("http://127.0.0.1:11434/api/tags", timeout=2)' >/dev/null 2>&1; then
    printf 'Ollama is not responding at http://127.0.0.1:11434. Start Ollama, then run this script again.\n' >&2
    printf 'For a manual start, run: ollama serve\n' >&2
    exit 1
fi

if ! ollama list | awk -v model="$MODEL" 'NR > 1 && $1 == model { found=1 } END { exit !found }'; then
    printf 'Downloading Ollama model %s...\n' "$MODEL"
    ollama pull "$MODEL"
fi

sudo -v
if command -v nmcli >/dev/null 2>&1 && [[ ! -f "$NETWORK_STATE_FILE" ]]; then
    nm_managed="$(nmcli -g GENERAL.NM-MANAGED device show "$IFACE" 2>/dev/null || true)"
    if [[ "$nm_managed" == "yes" ]]; then
        printf '%s yes\n' "$IFACE" >"$NETWORK_STATE_FILE"
        sudo nmcli device set "$IFACE" managed no
        printf 'Temporarily released %s from NetworkManager so its direct-link address stays assigned.\n' "$IFACE"
    fi
fi
sudo ip link set dev "$IFACE" up
sudo ip address replace "$HOST_ADDRESS/24" dev "$IFACE"

dhcp_running=0
if sudo test -f "$DNSMASQ_PID_FILE"; then
    old_pid="$(sudo cat "$DNSMASQ_PID_FILE")"
    if [[ "$old_pid" =~ ^[0-9]+$ ]] && sudo kill -0 "$old_pid" 2>/dev/null; then
        old_args="$(sudo ps -p "$old_pid" -o args=)"
        if [[ "$old_args" == *"--interface=$IFACE"* && "$old_args" == *"--listen-address=$HOST_ADDRESS"* ]]; then
            dhcp_running=1
            printf 'Reusing the existing AOS-link DHCP server (PID %s).\n' "$old_pid"
        else
            printf 'PID %s exists but is not the expected AOS-link DHCP server; inspect %s before proceeding.\n' "$old_pid" "$DNSMASQ_PID_FILE" >&2
            exit 1
        fi
    else
        sudo rm -f "$DNSMASQ_PID_FILE"
    fi
fi

if [[ "$dhcp_running" -eq 0 ]]; then
    sudo dnsmasq \
        --conf-file=/dev/null \
        --interface="$IFACE" \
        --bind-interfaces \
        --listen-address="$HOST_ADDRESS" \
        --dhcp-range="$DHCP_RANGE_START,$DHCP_RANGE_END,255.255.255.0,12h" \
        --dhcp-leasefile="$DHCP_LEASE_FILE" \
        --pid-file="$DNSMASQ_PID_FILE" \
        --port=0 \
        --no-resolv \
        --no-hosts
fi

if ! python3 -c 'import urllib.request; urllib.request.urlopen("http://192.168.77.1:11434/api/tags", timeout=2)' >/dev/null 2>&1; then
    if [[ -f "$RELAY_PID_FILE" ]]; then
        old_relay_pid="$(cat "$RELAY_PID_FILE")"
        if [[ "$old_relay_pid" =~ ^[0-9]+$ ]] && kill -0 "$old_relay_pid" 2>/dev/null; then
            old_relay_args="$(ps -p "$old_relay_pid" -o args=)"
            if [[ "$old_relay_args" == *"ollama_lan_relay.py"* ]]; then
                kill "$old_relay_pid" 2>/dev/null || true
            else
                printf 'Relay PID file points to an unrelated process; inspect %s.\n' "$RELAY_PID_FILE" >&2
                exit 1
            fi
        fi
        rm -f "$RELAY_PID_FILE"
    fi
    nohup python3 "$ROOT_DIR/tools/ollama_lan_relay.py" --bind-address "$HOST_ADDRESS" \
        >"$RELAY_LOG_FILE" 2>&1 &
    relay_pid=$!
    printf '%s\n' "$relay_pid" >"$RELAY_PID_FILE"
    printf 'Started the local-only Ollama relay (PID %s); AOS endpoint is %s:11434.\n' "$relay_pid" "$HOST_ADDRESS"
else
    printf 'Ollama relay is already reachable at %s:11434.\n' "$HOST_ADDRESS"
fi

printf '\nLOQ link ready: %s/24 on %s; DHCP range %s-%s.\n' "$HOST_ADDRESS" "$IFACE" "$DHCP_RANGE_START" "$DHCP_RANGE_END"
printf 'Boot AOS and read the address printed by its network setup (or run ifconfig).\n'

if [[ -z "$AOS_IP" ]]; then
    read -r -p 'AOS IPv4 address: ' AOS_IP
fi

if ! python3 - "$AOS_IP" <<'PY'
import ipaddress
import sys

try:
    target = ipaddress.ip_address(sys.argv[1])
except ValueError:
    raise SystemExit(1)

if target not in ipaddress.ip_network("192.168.77.0/24") or target == ipaddress.ip_address("192.168.77.1"):
    raise SystemExit(1)
PY
then
    printf 'Invalid AOS address for this direct link: %s (expected 192.168.77.2-254).\n' "$AOS_IP" >&2
    exit 2
fi

printf '\nThe AOS kernel will plan and execute the task using its native agent.\n'
read -r -p 'What should AOS do? (leave blank to enter it at the AOS console): ' TASK_GOAL
if [[ -z "${TASK_GOAL//[[:space:]]/}" ]]; then
    printf 'At the AOS prompt, enter: ask <goal>\n'
    exit 0
fi

printf 'Sending your goal to the AOS agent at %s:9001.\n' "$AOS_IP"
cd "$ROOT_DIR"
exec python3 tools/aos_command_bridge.py --host "$AOS_IP" --port 9001 --goal "$TASK_GOAL"
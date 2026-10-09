# Direct Ethernet Setup: AOS PC + Lenovo LOQ

This setup connects a bare-metal AOS PC directly to the Lenovo LOQ with one Ethernet cable. Ollama and `gemma3:4b` run on the LOQ; AOS requests a DHCP address from the LOQ and listens for commands on TCP port 9001. No router or cloud model service is needed.

In this guide, **server** means the Lenovo LOQ: it runs Ollama and gives the AOS PC its network address. The **AOS PC** is the bare-metal client: it runs the OS and receives commands from the LOQ.

```text
Bare-metal AOS PC                         Lenovo LOQ
DHCP client                               Ollama: 127.0.0.1:11434
TCP command listener: 9001  <--- LAN ---> Ethernet: 192.168.77.1/24
Ollama relay: 11434                       DHCP server: dnsmasq
DHCP address: 192.168.77.10-30
```

## Requirements

- A working wired Ethernet port on each computer and a regular Ethernet cable. Modern ports normally support auto MDI-X, so a crossover cable is usually unnecessary.
- AOS built from this repository and booted on the target PC.
- AOS's Ethernet driver must support the target PC's NIC. The current tree has RTL8169 and Intel e1000 drivers; an unsupported onboard NIC will not gain support from this setup.
- Linux on the LOQ, with `ip`, `sudo`, `dnsmasq`, Python 3, and Ollama installed. Install dnsmasq using the LOQ distribution's package manager if it is missing.
- Ollama running locally on the LOQ. Setup exposes it only through a relay bound to the direct-link address `192.168.77.1`, not Wi-Fi or the public internet.

## First-time setup: follow these steps in order

### A. Prepare the LOQ server

Open a terminal on the LOQ and make sure it has an AOS project checkout containing both `setup.sh` and `tools/aos_command_bridge.py`. If the project is already on the LOQ, change into that repository directory. Confirm the files and basic tools exist:

```bash
test -f setup.sh && test -f tools/aos_command_bridge.py
ip -brief link
python3 --version
ollama --version
```

If the project is not on the LOQ, copy the checkout that contains these setup files from the development PC first. Run all following project commands from that checkout's root directory.

On Ubuntu, Debian, or Kali, install the network utilities if they are missing:

```bash
sudo apt update
sudo apt install -y dnsmasq iproute2 python3
```

Install Ollama using its official installation instructions if `ollama --version` is not recognized. Start the Ollama service, or run `ollama serve` in a separate terminal and leave that terminal open. Then download the model once:

```bash
ollama pull gemma3:4b
```

### B. Connect the machines

Connect one Ethernet cable directly between the LOQ's wired Ethernet port and the AOS PC's Ethernet port. Keep the LOQ connected to the internet over Wi-Fi if you need internet access; do not use Wi-Fi as the interface given to the setup script.

On the LOQ, list network interfaces:

```bash
ip -brief link
```

Identify the wired interface by its name and link state, commonly `enp3s0`, `eno1`, or similar. Use that exact name in the setup command. Never use `lo` or a `wl*`/`wlan*` Wi-Fi interface.

### C. Start DHCP and the command bridge

From the repository root on the LOQ, start setup with the wired interface name. Example:

```bash
chmod +x setup.sh
./setup.sh enp3s0
```

Replace `enp3s0` with the interface identified in step B. Review the interface printed by the script, type `y` to approve, and enter the LOQ's sudo password if asked. The script temporarily releases the wired interface from NetworkManager, assigns the LOQ `192.168.77.1/24`, and starts DHCP for the cable. This prevents NetworkManager from replacing the direct-link address while AOS boots. Do not close this terminal.

When the script prints `AOS IPv4 address:`, leave it waiting and boot the AOS PC from its AOS USB/ISO with the Ethernet cable still attached. At boot, AOS may print `DHCP-pending` before its network driver gets a lease; wait for the DHCP/IP message, then run `ifconfig` at the AOS prompt if you need to check the address. The AOS address should be between `192.168.77.10` and `192.168.77.30`. Type that address into the waiting LOQ terminal and press Enter.

The bridge now calls the local Gemma model and relays its bounded command to AOS at `<AOS-IP>:9001`. AOS's built-in `agent_task` can also reach the LOQ model at `192.168.77.1:11434` through the direct-link-only relay. For example, if AOS shows `192.168.77.10`, the command target is `192.168.77.10:9001`.

### D. Install and test a runtime command

After booting the rebuilt ISO, install the allowlisted `pwd` runtime command from the LOQ:

```bash
python3 tools/agent_test.py --host 192.168.77.10 --inject-runtime-pwd
```

Replace the address with the one AOS currently reports. The JIT driver writes the allowlisted runtime program `pwd:print_current_directory` to AOS's FAT32 volume; the shell loads and interprets that program when `pwd` is invoked, including after a reboot. This requires the AHCI driver to find an existing writable FAT32 partition. AOS never formats or repartitions the disk. If no usable FAT32 partition is found, it reports that storage is a volatile RAM disk and the installed program will not survive reboot.

### E. Stop the session

Press Ctrl+C in the LOQ terminal to stop the command bridge. Stop the DHCP service with:

```bash
./setup.sh --stop-dhcp
```

The script restores NetworkManager control of the wired interface when it was managed before setup. Reconnect it to another network using your normal network settings.

## Quick address reference

| Machine/interface | Address or service |
| --- | --- |
| Lenovo LOQ wired Ethernet | `192.168.77.1/24` |
| AOS PC Ethernet | DHCP lease `192.168.77.10`-`192.168.77.30` |
| AOS remote command listener | AOS address, TCP port `9001` |
| Ollama API | LOQ localhost `127.0.0.1:11434`; AOS-only relay `192.168.77.1:11434` |

This is a private link between the two physical machines. It does not configure internet sharing. AOS's DHCP address can change after reboot, so check `ifconfig` again each time and enter the current address into the LOQ setup prompt.

QEMU uses its own virtual network and may show an address such as `10.0.2.15`. That is expected in QEMU and is not the address to enter for the direct Ethernet cable. Use the `192.168.77.x` address reported when AOS is booted on the physical PC.

## Troubleshooting

- **No AOS address / `DHCP-pending`:** Check the cable and link lights, confirm the chosen LOQ interface is wired, and check that AOS detected a supported RTL8169 or e1000 NIC. AOS needs a DHCP lease before the address is useful.
- **`dnsmasq` fails to start:** Another DHCP server may already be using the interface, or the selected interface/address is wrong. Do not run two DHCP servers on the same link. Check the error, stop any stale AOS-link instance with `./setup.sh --stop-dhcp`, and retry.
- **Connection refused or timed out on port 9001:** Confirm the AOS address with `ifconfig`, verify AOS printed that it started the listener, and ensure the cable is connected to the same interface configured on the LOQ. The bridge's default port is 9001.
- **AOS `agent_task` cannot reach Ollama:** Ensure the setup terminal is running and the relay reports `192.168.77.1:11434`. The relay forwards only on the direct Ethernet interface to Ollama at `127.0.0.1:11434`.
- **Model not found:** The script pulls the selected model if necessary. Set `HARVIS_MODEL` to another local Ollama model if desired, for example `HARVIS_MODEL=gemma3:4b ./setup.sh enp3s0`.

## Security note

The AOS TCP command listener on port 9001 has no authentication or encryption. The bridge applies a command allowlist, but that is not a substitute for server-side authentication. Use this only on the direct, physically controlled cable between the two computers; do not expose port 9001 to Wi-Fi, a shared LAN, or the public internet. Treat all remote commands as experimental until the AOS listener enforces its own complete allowlist.
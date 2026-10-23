#!/bin/sh
# Host side of the phone's USB Ethernet (kernel sweetusb=2, patches 0073-0075).
#   tools/sweet-usbnet.sh [LOGFILE]
#   tools/sweet-usbnet.sh --stage        print the ONE sudo line that configures the interface (no wait, no sudo here),
#                                        for the user to run once the phone has enumerated (tools/xrun --usbnet does this)
# Waits for the phone's CDC-ECM interface (host MAC 02:00:5e:77:ee:01),
# gives it 192.168.7.1/24 (the phone is 192.168.7.2), then prints the live
# kernel console the phone streams to UDP port 5140 (and appends it to
# LOGFILE if given). Needs sudo for the address; tools/usbnet-log.py is the root-free listener (xrun uses it).
set -eu

# Finds the interface by the phone's MAC itself, so the line can be staged before the interface exists.
STAGE="sudo sh -c 'i=\$(ip -o link | grep -i 02:00:5e:77:ee:01 | cut -d: -f2 | cut -d@ -f1 | xargs); ip addr replace 192.168.7.1/24 dev \$i && ip link set \$i up'"
if [ "${1:-}" = "--stage" ]; then
	echo "$STAGE"
	exit 0
fi

if [ "${1:-}" = "--stage-udev" ]; then
	# ONE-TIME root install: afterwards the phone's USB-Ethernet interface gets 192.168.7.1/24 by itself on every boot.
	echo "sudo install -m644 $(cd "$(dirname "$0")" && pwd)/90-pixel-usbnet.rules /etc/udev/rules.d/ && sudo udevadm control --reload"
	exit 0
fi
if [ "${1:-}" = "-h" ] || [ "${1:-}" = "--help" ]; then
	echo "Usage: $(basename "$0") [LOGFILE]"
	echo "Wait for the phone's USB Ethernet interface and stream the live kernel UDP console."
	exit 0
fi

mac=02:00:5e:77:ee:01
echo "waiting for the phone's USB Ethernet ($mac)..." >&2
until ip -o link | grep -qi "$mac"; do sleep 1; done
ifname=$(ip -o link | grep -i "$mac" | awk -F': ' '{print $2}' | cut -d@ -f1)
nmcli dev set "$ifname" managed no 2>/dev/null || true
echo "root step (or stage it yourself: $0 --stage): $STAGE" >&2
sudo ip addr replace 192.168.7.1/24 dev "$ifname"
sudo ip link set "$ifname" up
echo "$ifname up: 192.168.7.1 <-> phone 192.168.7.2; live log on udp/5140" >&2

listen_udp() {
	port=5140
	if command -v socat >/dev/null 2>&1; then
		socat -u "udp-recv:$port,reuseaddr" -
	elif command -v ncat >/dev/null 2>&1; then
		ncat -l -u -k "$port"
	elif nc -h 2>&1 | grep -q -- '-k'; then
		nc -ukl "$port"
	else
		while true; do
			nc -u -l -p "$port" 2>/dev/null || nc -u -l "$port" 2>/dev/null || break
		done
	fi
}

if [ $# -ge 1 ]; then
	listen_udp | tee -a "$1"
else
	listen_udp
fi

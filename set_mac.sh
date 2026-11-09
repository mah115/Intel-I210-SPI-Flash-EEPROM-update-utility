#!/usr/bin/env bash
# set_mac.sh - write a MAC address into a programmed I210's NVM via ethtool.
# Usage: sudo ./set_mac.sh <interface> <mac>
#   e.g. sudo ./set_mac.sh enP7p1s0 02:11:22:33:44:55
# Requires the I210 to enumerate as 8086:1533 with the igb driver bound.
set -euo pipefail

if [[ $# -ne 2 ]]; then
    echo "Usage: sudo $0 <interface> <mac>" >&2
    exit 1
fi
IFACE=$1
MAC=${2,,}

if [[ ! $MAC =~ ^([0-9a-f]{2}:){5}[0-9a-f]{2}$ ]]; then
    echo "Invalid MAC '$2' (expected xx:xx:xx:xx:xx:xx)" >&2
    exit 1
fi
FIRST=$((16#${MAC:0:2}))
if (( FIRST & 1 )); then
    echo "Invalid MAC: first byte is odd (multicast bit set)" >&2
    exit 1
fi

DRIVER=$(ethtool -i "$IFACE" 2>/dev/null | awk '/^driver:/{print $2}')
if [[ "$DRIVER" != "igb" ]]; then
    echo "$IFACE is not driven by igb (driver: '${DRIVER:-none}')" >&2
    exit 1
fi

IFS=: read -r -a BYTES <<< "$MAC"
for i in 0 1 2 3 4 5; do
    ethtool -E "$IFACE" magic 0x15338086 offset "$i" value "0x${BYTES[$i]}"
done

echo "Written. NVM bytes 0-5 now read:"
ethtool -e "$IFACE" offset 0 length 6
echo "Cold boot (remove input power) to load the new MAC, then check: ip link show $IFACE"

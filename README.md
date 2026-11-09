# i210-nvm-tool

Programs the SPI flash behind a **blank Intel I210** Ethernet controller over PCIe, on ARM64 Linux, without Intel's EEUPDATE/LANConf tools.

A blank I210 enumerates as **8086:1531** ("I210 Gigabit Unprogrammed"). The Linux `igb` driver won't bind to it, and Intel's tools time out on blank flash because they go through the I210's internal flash manager. `i210flash` instead bit-bangs raw SPI through the I210's FLA (Flash Access) register in BAR0, the same method flashrom's `nicintel_spi` uses. It maps BAR0 through sysfs (`resource0`), so it gets the correct translated address on Tegra PCIe, where flashrom needs source patches.

After the first image is written and the board is cold-booted, the I210 enumerates as **8086:1533**, `igb` binds, and further NVM changes (such as the MAC address) are made with `ethtool -E` through `set_mac.sh`.

## Validated configuration

| Item | Value |
|---|---|
| Host | Jetson Orin NX 16GB on Bedrock carrier BR20166-00 Rev A, JetPack 7 |
| Controller | Intel I210-IT, PCIe address `0007:01:00.0` on that board |
| Flash | Winbond W25Q80DVUXIE (8 Mbit, JEDEC ID `EF 40 14`) |
| Image | `Dell_Ford_I210_Copper_NOMNG_8Mb_A2_800007E6.bin` (1 MiB) |

Other flash parts should work if they use standard SPI opcodes (0x9F, 0x05, 0x35, 0x06, 0x01, 0xC7, 0x02, 0x03). The tool assumes a 1 MiB flash (`FLASH_SIZE` in the source); change that for other densities.

## Contents

| File | Purpose |
|---|---|
| `i210flash.c` | Flash programmer (C, no external libraries) |
| `Makefile` | Builds `i210flash` |
| `set_mac.sh` | Writes a MAC address into a programmed I210 via `ethtool` |
| `README.md` | This file |

## Dependencies

On the Jetson (Ubuntu/L4T):

```
sudo apt update
sudo apt install build-essential pciutils ethtool
```

- `build-essential`: gcc and make to build the tool
- `pciutils`: `lspci`, to find the device and check its ID
- `ethtool`: to set the MAC address and check link after programming

No kernel modules, libpci, or `/dev/mem` access are needed. The tool must run as root.

## NVM image requirements

- **Size must be ≤ the flash size** (1 MiB for the W25Q80DV). Use an image built for 8 Mbit or smaller flash (`…_8Mb_…` or `…_4Mb_…`).
- **Do not truncate a larger image.** For example, the 2 MiB `OEM_Beaver_Lake` image from Intel's driver pack has real data above 1 MiB and will not work.
- Use the **Copper** variant for I210-IT/AT; SerDes/SGMII images are for other board designs.
- The preferred source is Intel's `Dev_Start_I210_Copper_NOMNG_8Mb_A2` image from the I210 production NVM image package (Intel document 513655, Resource & Design Center), or from an Intel distributor FAE. The Dell image above also works.

Check an image before writing it (no hardware needed):

```
./i210flash info IMAGE.bin
```

Expect `Device ID: 0x1533` and `Checksum: … (valid)`. The MAC in the image is a placeholder and is set later.

## Programming procedure

### 1. Build

```
make
```

### 2. Find the I210

```
lspci -D -nn | grep -i 8086
```

Use the full address in the first column (e.g. `0007:01:00.0`) wherever `BDF` appears below. A blank part shows `[8086:1531]`.

If the device is bound to `igb` (an already-programmed part), the tool will refuse to run and print the unbind command.

### 3. Test access (read-only)

```
sudo ./i210flash BDF id
```

Expected output:

```
STATUS=0x00380780 EEC=0x00082a00 FLA=0x0000000a
JEDEC ID: EF 40 14  (CS polarity FL_CE=1 releases)
SR1=00 SR2=00
```

- `STATUS` must not be `0xffffffff`; if it is, the BAR is not reachable.
- `JEDEC ID` must match your flash part. If not, stop and check flash power and wiring.

### 4. Write the image

```
sudo ./i210flash BDF write IMAGE.bin
```

This clears write protection, erases the chip, programs it, and reads it back. It typically takes a minute or two. Do not interrupt it. Success ends with `Verify OK`.

### 5. Cold boot (required)

**A soft reboot is not enough.** The I210 only reads its NVM at power-on, and its 3.3 V rail stays up during a reboot.

1. `sudo poweroff`
2. Remove DC input power from the carrier board.
3. Wait about 10 seconds.
4. Restore power and boot.

### 6. Confirm

```
lspci -nn | grep -i 8086                 # expect [8086:1533]
lspci -k -s BDF                          # expect "Kernel driver in use: igb"
ip link                                  # find the interface, e.g. enP7p1s0
```

### 7. Set the MAC address

```
sudo ./set_mac.sh enP7p1s0 02:11:22:33:44:55
```

Use a unique address for every board: one from Bedrock's OUI if assigned, otherwise a locally administered address (first byte `02`). The script refuses multicast addresses and non-`igb` interfaces.

Cold boot again, then confirm with `ip link show enP7p1s0`.

### 8. Link test

With a cable connected to a gigabit switch:

```
ethtool enP7p1s0      # expect Speed: 1000Mb/s, Duplex: Full, Link detected: yes
```

## Command reference

```
./i210flash info   IMAGE.bin        inspect an image file (no hardware)
sudo ./i210flash BDF id             read JEDEC ID and status registers
sudo ./i210flash BDF read   OUT.bin dump the whole flash to a file
sudo ./i210flash BDF write  IMG.bin unprotect, erase, program, verify
sudo ./i210flash BDF verify IMG.bin compare flash contents to a file
```

## Troubleshooting

**Still 8086:1531 after a successful write.** Check that the board was fully powered off; this was the cause on the first board. Then run `sudo ./i210flash BDF verify IMAGE.bin`:
- If verify passes, the flash is correct and the I210 is rejecting or failing to read it at power-up. Check the pin 12 strap (below), check that the flash is powered from the same 3.3 V rail as the I210, and scope CS#/SO at power-on.
- If verify fails, rewrite and check for errors in the output.

**`No flash response with either CS polarity`.** The SPI path isn't working. Check flash power, the FLSH_* connections, and that nothing else drives the bus.

**`Protection bits did not clear`.** The flash's /WP pin is probably held low. It must be tied high.

**`Device … is bound to driver 'igb'`.** The part is already programmed. Use `set_mac.sh` / `ethtool -E` for normal changes. For a raw rewrite, unbind with the printed command first.

## Hardware notes for future revisions

- **Pin 12 (FLSH_SO) security strap.** The I210 samples this pin at power-up. On Rev A it connects directly to the flash with no resistor, so it reads high and NVM security stays enabled. Add a ~3.3 kΩ pull-down to GND on Rev B, as Intel support recommends. If a future image enables security, raw re-flashing may be blocked (FLA bit 6, `FL_LOCKED`, set) until this pull-down is fitted.
- **Power sequencing.** Power the flash from the same rail as the I210, so the flash is ready when the I210 reads it at power-on.
- **Flash /WP and /HOLD.** Tie both high.

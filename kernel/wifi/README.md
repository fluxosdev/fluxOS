# AX210 driver development

The AX210 transport and station implementation now build and link into both
kernel variants. Hardware bring-up and connectivity have not been verified
on an AX210. The transport uses polling; it does not require routed INTx/MSI.

The existing code provides an 802.11 station layer, WPA2-PSK handshake,
firmware/PNVM parsing, firmware initialization commands, a Wi-Fi syscall,
and the `wifi` command. The bundled firmware is
`iwlwifi-ty-a0-gf-a0-89.ucode` with `iwlwifi-ty-a0-gf-a0.pnvm`.
The intended initial connection mode is legacy 2.4/5 GHz with open or
WPA2-PSK networks; this is not 6 GHz, WPA3, or Wi-Fi 6 support.

## Firmware ownership

`iwl_fw_parse` borrows image-loader and code-section pointers from its
input buffer. The transport must copy those sections before the buffer
is released after ALIVE. Command-version records are owned by `iwl_fw`
because initialization and station commands query them after ALIVE.

Firmware TLVs must include their four-byte alignment padding. Truncated
records and partial command-version entries are rejected. PNVM parsing
returns a negative error for damaged records or too many chunks; callers
must not pass partial results to the device.

## Hardware validation still required

Test on an AX210, first reaching ALIVE/NVM, then scan, open-network data,
WPA2, DHCP, reconnect, and failure recovery. Host tests and ordinary QEMU
network devices cannot validate AX210 register or firmware behavior.

Current limits: passive scanning while idle, legacy 2.4/5 GHz frames,
no HT aggregation, and no suspend/resume integration. On passive-only and
DFS channels (in the world domain, all of 5 GHz) it joins an AP the scan
heard but never initiates transmission itself; the firmware waits to
receive there before it sends.
Firmware crashes require a reboot. Failed DMA-stop acknowledgement retains
buffers until reboot. This implementation must not be described as proven
working connectivity until tested on hardware.

## Trying it without a card

The kernel option `wifi.sim` (used only when no real card is found)
registers a simulated card (`wifi_sim.c`): a scan hears six networks
(WPA2, open, WPA3-only, 5 GHz, and one hiding its name), authentication
and association are answered, and the WPA2 ones run the access point's side
of the 4-way handshake with the password `correct horse`, so a wrong
password fails as it would. No DHCP server answers. It exercises `wifi.c`,
`wpa.c`, the `wifi` command and the desktop's Wireless Networks under QEMU;
it says nothing about the AX210.

## Verification

Run from the repository root:

```sh
make build/tests/kernel-wifi
build/tests/kernel-wifi
make build/tests/kernel-iwl-trans build/tests/kernel-iwl-sta
build/tests/kernel-iwl-trans
build/tests/kernel-iwl-sta
make build/fluxos.kernel build/fluxos-text.kernel
make build/k/kernel/wifi/iwl_fw.o build/k/kernel/wifi/iwl_mvm.o
```

Tests include the shipped firmware and all four PNVM SKU sections, malformed
TLV boundaries, chunk limits, command metadata lifetime, crypto vectors,
WPA2 handshake cases, command completion correlation, queue wraparound,
RX length validation, CCMP replay checks, and TX formatting. These are software tests, not hardware validation.

The transport regression also models reset losing NIC ownership, checks
the required L0s-disable setting, exercises command sequences through both
8-bit and 16-bit rollover, and rejects all fragments of a split RX packet
even when the final continuation arrives in a later poll.

If `8086:2725` is absent from PCI enumeration, firmware loading has not
started. The `pci-net` diagnostics then list every network controller and
every PCI bridge, with each bridge's bus numbers, memory windows, PCIe link
status and, for a port with a slot, whether a card is present. If the card
is found but its BAR0 cannot be mapped or reads back as all ones, they list
the card and only the bridges above it, so BAR0 can be checked against each
window. The probe wakes the card from D1-D3hot (putting back its BARs) and
turns on memory decoding and bus mastering on the bridges above it.
Transport initialization fixes only apply once the endpoint is visible;
they do not establish recovery of a missing PCIe link.

## Protocol references

Use the upstream Intel driver as the hardware/firmware reference:

- [Firmware TLV definitions (Linux v6.12)](https://github.com/torvalds/linux/blob/v6.12/drivers/net/wireless/intel/iwlwifi/fw/file.h)
- [AX210 configuration](https://github.com/torvalds/linux/blob/master/drivers/net/wireless/intel/iwlwifi/cfg/ax210.c)
- [PCIe transport](https://github.com/torvalds/linux/tree/v6.12/drivers/net/wireless/intel/iwlwifi/pcie)

Preserve upstream license and attribution requirements when importing code.

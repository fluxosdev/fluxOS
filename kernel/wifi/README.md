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
no DFS transmission, no HT aggregation, and no suspend/resume integration.
Firmware crashes require a reboot. Failed DMA-stop acknowledgement retains
buffers until reboot. This implementation must not be described as proven
working connectivity until tested on hardware.

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

## Protocol references

Use the upstream Intel driver as the hardware/firmware reference:

- [Firmware TLV definitions (Linux v6.12)](https://github.com/torvalds/linux/blob/v6.12/drivers/net/wireless/intel/iwlwifi/fw/file.h)
- [AX210 configuration](https://github.com/torvalds/linux/blob/master/drivers/net/wireless/intel/iwlwifi/cfg/ax210.c)
- [PCIe transport](https://github.com/torvalds/linux/tree/v6.12/drivers/net/wireless/intel/iwlwifi/pcie)

Preserve upstream license and attribution requirements when importing code.

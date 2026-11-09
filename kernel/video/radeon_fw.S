/*
 * AMD's command processor microcode for the R700 chips, from the
 * linux-firmware collection (firmware/radeon; its license is
 * firmware/radeon/LICENSE.radeon, also /usr/share/doc/firmware in the
 * image). The PFP is the prefetch parser, the ME the micro engine; the
 * RV740 runs the RV730's, and the RV730's PFP is the RV710's. The Llano
 * APUs' graphics (Sumo) have their own; SUMO2_me is the same file as
 * SUMO_me, so only SUMO2's PFP is here. The words are big-endian.
 */
.section .rodata
.balign 4

.global radeon_fw_rv770_pfp
radeon_fw_rv770_pfp:
    .incbin "firmware/radeon/RV770_pfp.bin"
.global radeon_fw_rv770_me
radeon_fw_rv770_me:
    .incbin "firmware/radeon/RV770_me.bin"
.global radeon_fw_rv730_me
radeon_fw_rv730_me:
    .incbin "firmware/radeon/RV730_me.bin"
.global radeon_fw_rv710_pfp
radeon_fw_rv710_pfp:
    .incbin "firmware/radeon/RV710_pfp.bin"
.global radeon_fw_rv710_me
radeon_fw_rv710_me:
    .incbin "firmware/radeon/RV710_me.bin"

.global radeon_fw_sumo_pfp
radeon_fw_sumo_pfp:
    .incbin "firmware/radeon/SUMO_pfp.bin"
.global radeon_fw_sumo_me
radeon_fw_sumo_me:
    .incbin "firmware/radeon/SUMO_me.bin"
.global radeon_fw_sumo2_pfp
radeon_fw_sumo2_pfp:
    .incbin "firmware/radeon/SUMO2_pfp.bin"

.section .note.GNU-stack, "", @progbits

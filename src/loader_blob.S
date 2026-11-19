/* wl82loader.bin (JieLi's WL82 flash loader, from kagaimiq/jl-uboot-tool, sha256 d41da612...5376),
   embedded under the symbol names jieli_uboot.c expects from ESP-IDF's EMBED_FILES. */
    .section .rodata.wl82loader, "a"
    .balign 4
    .global _binary_wl82loader_bin_start
    .global _binary_wl82loader_bin_end
_binary_wl82loader_bin_start:
    .incbin "src/wl82loader.bin"
_binary_wl82loader_bin_end:

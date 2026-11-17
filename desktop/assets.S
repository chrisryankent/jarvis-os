/* Pictures built into the kernel (see scripts/gen_wallpapers.py). */
    .section .rodata
    .macro asset name, file
    .global asset_\name
    .global asset_\name\()_end
    .balign 16
asset_\name:
    .incbin "\file"
asset_\name\()_end:
    .endm

    asset cosmos_jpg, "desktop/assets/cosmos.jpg"
    asset nebula_jpg, "desktop/assets/nebula.jpg"

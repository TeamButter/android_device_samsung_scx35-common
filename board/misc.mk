# Include an expanded selection of fonts
EXTENDED_FONT_FOOTPRINT := true

# Bionic
TARGET_NEEDS_PLATFORM_TEXT_RELOCATIONS := true

# Shims
TARGET_LD_SHIM_LIBS := \
    /system/bin/gpsd|libgps_shim.so    

# PowerHAL
TARGET_POWERHAL_VARIANT := scx35

# Build system
WITHOUT_CHECK_API := true

# Malloc implementation
MALLOC_SVELTE := true

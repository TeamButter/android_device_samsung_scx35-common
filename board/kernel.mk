# Kernel
BOARD_CUSTOM_BOOTIMG_MK := device/samsung/scx35-common/mkbootimg.mk
BOARD_KERNEL_BASE := 0x00000000
BOARD_KERNEL_TAGS_OFFSET := 0x01d88000
BOARD_RAMDISK_OFFSET := 0x01000000
BOARD_KERNEL_IMAGE_NAME := zImage
BOARD_KERNEL_PAGESIZE := 2048
BOARD_MKBOOTIMG_ARGS := --kernel_offset 0x00008000
BOARD_KERNEL_SEPARATED_DT := true
TARGET_KERNEL_CROSS_COMPILE_PREFIX := arm-eabi-
KERNEL_TOOLCHAIN := $(ANDROID_BUILD_TOP)/prebuilts/gcc/linux-x86/arm/arm-eabi-4.8/bin

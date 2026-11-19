# fm1-stm32-unbrick - bare-metal build for the STM32F4DISCOVERY (STM32F407VG)
#   sh tools/fetch_deps.sh && make      (and `make selftest` for the USB-host self-test build)
DEPS ?= third_party
TUSB := $(DEPS)/tinyusb
RTT  := $(DEPS)/RTT

CC      := arm-none-eabi-gcc
OBJCOPY := arm-none-eabi-objcopy
SIZE    := arm-none-eabi-size

TARGET := fm1-stm32-unbrick
BUILD  := build

MCU := -mcpu=cortex-m4 -mthumb -mfloat-abi=hard -mfpu=fpv4-sp-d16

DEFS := -DSTM32F407xx -DHSE_VALUE=8000000 \
        -DRTT_USE_ASM=0 -DSEGGER_RTT_MAX_NUM_UP_BUFFERS=1 -DSEGGER_RTT_MAX_NUM_DOWN_BUFFERS=1 \
        -DBUFFER_SIZE_UP=16384 -DBUFFER_SIZE_DOWN=16384 \
        -DSEGGER_RTT_MODE_DEFAULT=SEGGER_RTT_MODE_NO_BLOCK_TRIM

INCS := -Isrc -I$(TUSB)/src -I$(RTT)/RTT -I$(RTT)/Config \
        -I$(DEPS)/cmsis_device_f4/Include -I$(DEPS)/cmsis_core/CMSIS/Core/Include

CFLAGS := $(MCU) $(DEFS) $(CFLAGS_EXTRA) $(INCS) -O2 -g3 -std=gnu11 -Wall -Wextra -Wno-unused-parameter \
          -ffunction-sections -fdata-sections -fno-common
LDFLAGS := $(MCU) -T stm32f407vg.ld -Wl,--gc-sections -Wl,-Map=$(BUILD)/$(TARGET).map \
           --specs=nano.specs --specs=nosys.specs -u _printf_float -Wl,--print-memory-usage

SRCS := src/main.c src/board.c src/recovery.c src/host.c src/proto.c src/log.c src/jieli_uboot.c \
        $(DEPS)/cmsis_device_f4/Source/Templates/system_stm32f4xx.c \
        $(TUSB)/src/tusb.c $(TUSB)/src/common/tusb_fifo.c $(TUSB)/src/host/usbh.c \
        $(TUSB)/src/portable/synopsys/dwc2/hcd_dwc2.c $(TUSB)/src/portable/synopsys/dwc2/dwc2_common.c \
        $(RTT)/RTT/SEGGER_RTT.c
ASRCS := $(DEPS)/cmsis_device_f4/Source/Templates/gcc/startup_stm32f407xx.s src/loader_blob.S

OBJS := $(addprefix $(BUILD)/,$(notdir $(SRCS:.c=.o))) $(addprefix $(BUILD)/,$(notdir $(addsuffix .o,$(basename $(ASRCS)))))
vpath %.c $(sort $(dir $(SRCS)))
vpath %.s $(sort $(dir $(ASRCS)))
vpath %.S $(sort $(dir $(ASRCS)))

all: $(BUILD)/$(TARGET).elf $(BUILD)/$(TARGET).bin

$(BUILD):
	mkdir -p $@

$(BUILD)/%.o: %.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.o: %.s | $(BUILD)
	$(CC) $(MCU) -c $< -o $@

$(BUILD)/%.o: %.S src/wl82loader.bin | $(BUILD)
	$(CC) $(MCU) -c $< -o $@

$(BUILD)/$(TARGET).elf: $(OBJS) stm32f407vg.ld
	$(CC) $(OBJS) $(LDFLAGS) -o $@
	$(SIZE) $@

$(BUILD)/$(TARGET).bin: $(BUILD)/$(TARGET).elf
	$(OBJCOPY) -O binary $< $@

# Self-test firmware: powers CN5 and talks to a USB flash drive instead of the FM-1 (never use it with the FM-1)
selftest:
	$(MAKE) BUILD=build-selftest TARGET=fm1-selftest CFLAGS_EXTRA=-DFM1_SELFTEST all
	mkdir -p build && cp build-selftest/fm1-selftest.elf build/

clean:
	rm -rf $(BUILD) build-selftest

.PHONY: all clean

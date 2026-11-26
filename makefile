# enosc3: Ensemble Oscillator firmware with Plaits engines.
#
#   make            -> main.bin  (flash at 0x08004000, after the bootloader)
#   make wav        -> main.wav  (audio bootloader file)
#   make size       -> sizes of the RAM image and of the flash
#
# The application is linked to run from RAM (lib/CMSIS/enosc3_ram.ld),
# deflate-compressed (tools/pack_image.py) and stored in flash together
# with a small loader (loader/) that unpacks it at boot. This leaves
# about twice the original 48K for code and tables.

APP = build/app
TARGET = main

SRCS = \
	data.cc \
	lib/easiglib/numtypes.cc \
	lib/easiglib/math.cc \
	lib/easiglib/dsp.cc \
	src/drivers/adc.cc \
	src/drivers/dac.cc \
	src/drivers/spi_adc.cc \
	src/drivers/leds.cc \
	src/drivers/buttons.cc \
	src/drivers/debug.cc \
	src/drivers/qspi_flash.cc \
	src/dynamic_data.cc \
	src/plaits_tables.cc \
	src/main.cc \

HAL = 	stm32f7xx_hal.o \
	stm32f7xx_hal_cortex.o \
	stm32f7xx_hal_gpio.o \
	stm32f7xx_hal_rcc.o \
	stm32f7xx_hal_rcc_ex.o \
	stm32f7xx_hal_dma.o \
	stm32f7xx_hal_sai.o \
	stm32f7xx_hal_sai_ex.o \
	stm32f7xx_hal_spi.o \
	stm32f7xx_hal_adc.o \
	stm32f7xx_hal_adc_ex.o \
	stm32f7xx_hal_pwr.o \
	stm32f7xx_hal_pwr_ex.o \
	stm32f7xx_hal_qspi.o \
	stm32f7xx_hal_tim.o \
	stm32f7xx_hal_tim_ex.o \

OPTIM ?= 2
TOOLCHAIN_DIR ?=
PYTHON ?= py -3

CXX = $(TOOLCHAIN_DIR)arm-none-eabi-g++
CC = $(TOOLCHAIN_DIR)arm-none-eabi-gcc
OBJCOPY = $(TOOLCHAIN_DIR)arm-none-eabi-objcopy
CMDSIZE = $(TOOLCHAIN_DIR)arm-none-eabi-size

CMSIS_DIR = lib/CMSIS/
HAL_DIR = lib/HAL/
EASIGLIB_DIR = lib/easiglib/

INC = -I . \
      -I src/ \
      -I src/drivers \
      -I $(EASIGLIB_DIR) \
      -I $(CMSIS_DIR) \
      -I $(HAL_DIR) \

APP_LDSCRIPT = $(CMSIS_DIR)enosc3_ram.ld
LOADER_LDSCRIPT = loader/loader.ld

ARCHFLAGS =  	-mcpu=cortex-m7 \
		-mthumb \
		-mfloat-abi=hard \
		-mfpu=fpv5-d16 \
		-mthumb-interwork \
		-mfp16-format=ieee \
		-DARM_MATH_CM7 \
		-DSTM32F730xx \

CPPFLAGS= $(INC)

CFLAGS= $(ARCHFLAGS) \
	-g \
	-ffast-math \
	-fdata-sections \
	-ffunction-sections \
	-ffreestanding \
	--param l1-cache-size=8 \
	--param l1-cache-line-size=32 \
	-DUSE_HAL_DRIVER \
	--specs=nano.specs

CXXFLAGS=$(CFLAGS) \
	-std=c++17 \
	-fno-rtti \
	-fno-exceptions \
	-Werror=return-type \
	-Wdouble-promotion \
	-Wno-register \

STARTUP = $(CMSIS_DIR)startup_stm32f730xx
SYSTEM = $(CMSIS_DIR)system_stm32f7xx

OBJS = $(STARTUP).o \
	$(SYSTEM).o \
	$(addprefix $(HAL_DIR), $(HAL)) \
	$(SRCS:.cc=.o)

LOADER_OBJS = loader/loader.o loader/inflate.o build/blob.o

OPTFLAG= -O$(OPTIM)

$(addprefix $(HAL_DIR), $(HAL)): OPTFLAG= -Os
# not in the audio path
src/drivers/qspi_flash.o data.o src/plaits_tables.o: OPTFLAG= -Os
# (the loader is built -Os in its own rules below: a target-specific
# OPTFLAG on it would leak to the whole application, a prerequisite)

all: $(TARGET).bin

# 1. the application, linked to run from RAM
$(APP).elf: data.hh $(OBJS) $(APP_LDSCRIPT)
	@mkdir -p build
	$(CC) $(CXXFLAGS) -T $(APP_LDSCRIPT) -Wl,--gc-sections -Wl,-Map,$(APP).map -o $@ $(OBJS)
	$(CMDSIZE) $@

$(APP).bin: $(APP).elf
	$(OBJCOPY) -O binary $< $@

# 2. compressed
build/blob.c: $(APP).bin tools/pack_image.py
	$(PYTHON) tools/pack_image.py $< $@

# 3. loader + compressed application = what goes to flash
$(TARGET).elf: $(LOADER_OBJS) $(LOADER_LDSCRIPT)
	$(CC) $(CFLAGS) -nostartfiles -nostdlib -T $(LOADER_LDSCRIPT) -Wl,--gc-sections -Wl,-Map,$(TARGET).map -o $@ $(LOADER_OBJS)

$(TARGET).bin: $(TARGET).elf
	$(OBJCOPY) -O binary $< $@
	$(CMDSIZE) $<
	@echo "flash used: `wc -c < $@` of 49152 bytes"

$(TARGET).hex: $(TARGET).elf
	$(OBJCOPY) --output-target=ihex $< $@

size: $(TARGET).bin
	$(CMDSIZE) -A $(APP).elf | head -20

%.o: %.s
	$(CC) -c -x assembler-with-cpp $(ASFLAGS) $< -o $@

data.cc data.hh: $(EASIGLIB_DIR)data_compiler.py data/data.py
	PYTHONPATH=$(EASIGLIB_DIR) python3 data/data.py

clean:
	rm -f $(OBJS) $(LOADER_OBJS) $(DEPS) $(TARGET).elf $(TARGET).bin $(TARGET).hex \
	$(TARGET).map build/*

wav: $(TARGET).bin
	PYTHONPATH='bootloader/:.' $(PYTHON) bootloader/stm_audio_bootloader/fsk/encoder.py \
		-s 22050 -b 16 -n 8 -z 4 -p 256 -g 16384 -k 1800 \
		$(TARGET).bin

# File dependencies:

DEPS = $(addsuffix .d, $(SRCS))
DEPFLAGS = -MMD -MP -MF $<.d

loader/%.o: loader/%.c
	$(CC) $(CFLAGS) -Os -I loader $(CPPFLAGS) -c $< -o $@

build/blob.o: build/blob.c
	$(CC) $(CFLAGS) -Os -c $< -o $@

%.o: %.c
	$(CC) $(CFLAGS) $(OPTFLAG) $(CPPFLAGS) -c $< -o $@

%.o: %.cc %.cc.d
	$(CXX) $(DEPFLAGS) $(OPTFLAG) $(CXXFLAGS) $(CPPFLAGS) -c $< -o $@

%.d: ;

-include $(DEPS)

.PRECIOUS: $(DEPS) $(OBJS) $(APP).elf data.cc data.hh
.PHONY: all clean wav size

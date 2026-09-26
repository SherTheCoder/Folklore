PROJECT   := KikkarProject_Appli
BUILD     := Appli/Debug
TARGET    := $(BUILD)/$(PROJECT).elf

CUBEIDE_TC := $(firstword $(wildcard \
  /Applications/STM32CubeIDE.app/Contents/Eclipse/plugins/com.st.stm32cube.ide.mcu.externaltools.gnu-tools-for-stm32.*/tools/bin))
TOOLCHAIN ?= $(CUBEIDE_TC)
CC        := $(TOOLCHAIN)/arm-none-eabi-gcc
SIZE      := $(TOOLCHAIN)/arm-none-eabi-size
OBJCOPY   := $(TOOLCHAIN)/arm-none-eabi-objcopy
OBJDUMP   := $(TOOLCHAIN)/arm-none-eabi-objdump

PYTHON    ?= python3
V         ?= 0
ifeq ($(V),0)
  Q := @
else
  Q :=
endif
POSES     ?= 0.0,1.0
SCALE     ?= 0.98
KIND_SCALE ?= --kind-scale leaf=1.6
EXTRA     ?=

LDSCRIPT  := Appli/STM32N657X0HXQ_LRUN.ld

ATLAS_BUDGET ?= 1368064

GEN_SRC   := Appli/Core/Src/flower_assets.c
FEATURE_DIRS := kws net stream opt app
FEATURE_SRCS := $(foreach d,$(FEATURE_DIRS),$(wildcard Appli/Core/Src/$(d)/*.c))
APP_SRCS  := $(filter-out $(GEN_SRC),$(wildcard Appli/Core/Src/*.c)) \
             $(FEATURE_SRCS) $(GEN_SRC)
HAL_SRCS  := $(wildcard Drivers/STM32N6xx_HAL_Driver/Src/*.c)
DSP_SRCS  := $(wildcard Drivers/CMSIS/DSP/Source/*.c)
ASM_SRCS  := Appli/Core/Startup/startup_stm32n657x0hxq.s \
             Appli/Core/Src/flower_atlas.S \
             Appli/Core/Src/kws/kws_model_blob.S

INCLUDES  := \
  -IAppli/Core/Inc \
  -ISecure_nsclib \
  -IDrivers/STM32N6xx_HAL_Driver/Inc \
  -IDrivers/STM32N6xx_HAL_Driver/Inc/Legacy \
  -IDrivers/CMSIS/Device/ST/STM32N6xx/Include \
  -IDrivers/CMSIS/Include \
  -IDrivers/CMSIS/DSP/Include \
  -IDrivers/CMSIS/DSP/PrivateInclude \
  -IDrivers/CMSIS/DSP/ComputeLibrary/Include

ARCH      := -mcpu=cortex-m55 -mfpu=fpv5-d16 -mfloat-abi=hard -mthumb
CFLAGS    := $(ARCH) -std=gnu11 -g3 -O2 -DDEBUG -DUSE_HAL_DRIVER -DSTM32N657xx $(EXTRA) \
             $(INCLUDES) -ffunction-sections -fdata-sections \
             -Wall -Wextra -fstack-usage -mcmse --specs=nano.specs
ASFLAGS   := $(ARCH) -g3 -DDEBUG -x assembler-with-cpp --specs=nano.specs \
             -DFLOWER_ATLAS_BIN='"Appli/Core/Src/flower_atlas.bin"' \
             -DKWS_MODEL_BIN='"Appli/Core/Src/kws/kws_model.bin"'
LDFLAGS   := $(ARCH) -T$(LDSCRIPT) --specs=nosys.specs --specs=nano.specs \
             -u _printf_float \
             -Wl,-Map=$(BUILD)/$(PROJECT).map -Wl,--gc-sections -static \
             -Wl,--cmse-implib -Wl,--out-implib=$(BUILD)/secure_nsclib.o \
             -Wl,--start-group -lc -lm -Wl,--end-group

VENDOR_CFLAGS := $(filter-out -Wextra,$(CFLAGS))

OBJDIR    := $(BUILD)/obj
APP_OBJS  := $(APP_SRCS:%.c=$(OBJDIR)/%.o)
HAL_OBJS  := $(HAL_SRCS:%.c=$(OBJDIR)/%.o)
DSP_OBJS  := $(DSP_SRCS:%.c=$(OBJDIR)/%.o)
ASM_OBJS  := $(ASM_SRCS:%=$(OBJDIR)/%.o)
OBJS      := $(APP_OBJS) $(HAL_OBJS) $(DSP_OBJS) $(ASM_OBJS)
DEPS      := $(OBJS:.o=.d)

GENERATED := Appli/Core/Inc/flower_assets.h Appli/Core/Src/flower_assets.c \
             Appli/Core/Src/flower_atlas.S Appli/Core/Src/flower_atlas.bin
STAMP     := $(BUILD)/.assets.stamp

MODEL_BIN   := Appli/Core/Src/kws/kws_model.bin
MODEL_ASM   := Appli/Core/Src/kws/kws_model_blob.S
MODEL_STAMP := $(BUILD)/.model.stamp
TFLITE    ?= $(firstword $(wildcard model/*.tflite))

.PHONY: all assets model size mapcheck figures clean distclean bin list

all: $(TARGET) size mapcheck

assets:
	@rm -f $(STAMP)
	@$(MAKE) --no-print-directory $(STAMP)

MANIFEST  := Kikkar_UI_Assets/out/manifest.json

ifneq ($(wildcard $(MANIFEST)),)
$(STAMP): tools/gen_flower_assets.py $(MANIFEST)
	@mkdir -p $(dir $@)
	$(PYTHON) tools/gen_flower_assets.py --poses $(POSES) --scale $(SCALE) --max-bytes $(ATLAS_BUDGET) $(KIND_SCALE)
	@touch $@
else
$(STAMP):
	@mkdir -p $(dir $@)
	@echo "  assets  no $(MANIFEST); using the checked-in atlas"
	@touch $@
endif

$(GENERATED): $(STAMP)

CFLAGSFILE := $(OBJDIR)/.cflags
$(shell mkdir -p $(OBJDIR); printf '%s' '$(CFLAGS)' | cmp -s - $(CFLAGSFILE) || printf '%s' '$(CFLAGS)' > $(CFLAGSFILE))

$(OBJDIR)/Appli/Core/Src/%.o: Appli/Core/Src/%.c $(STAMP) $(MODEL_STAMP) $(CFLAGSFILE)
	@mkdir -p $(dir $@)
	@echo "  CC      $<"
	$(Q)$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(OBJDIR)/Drivers/%.o: Drivers/%.c $(CFLAGSFILE)
	@mkdir -p $(dir $@)
	@echo "  CC[v]   $<"
	$(Q)$(CC) $(VENDOR_CFLAGS) -MMD -MP -c $< -o $@

$(OBJDIR)/%.s.o: %.s
	@mkdir -p $(dir $@)
	@echo "  AS      $<"
	$(Q)$(CC) $(ASFLAGS) -MMD -MP -c $< -o $@

$(OBJDIR)/Appli/Core/Src/flower_atlas.S.o: Appli/Core/Src/flower_atlas.S $(STAMP)
	@mkdir -p $(dir $@)
	@echo "  AS      $<"
	$(Q)$(CC) $(ASFLAGS) -c $< -o $@

$(OBJDIR)/Appli/Core/Src/kws/kws_model_blob.S.o: $(MODEL_ASM) $(MODEL_BIN)
	@mkdir -p $(dir $@)
	@echo "  AS      $<"
	$(Q)$(CC) $(ASFLAGS) -c $< -o $@

$(TARGET): $(OBJS) $(LDSCRIPT)
	@mkdir -p $(dir $@)
	@echo "  LD      $@"
	$(Q)$(CC) $(OBJS) $(LDFLAGS) -o $@

model:
	@rm -f $(MODEL_STAMP)
	@$(MAKE) --no-print-directory $(MODEL_STAMP)

$(MODEL_STAMP): tools/export_kws_model.py $(wildcard model/*.tflite)
	@mkdir -p $(dir $@)
	$(PYTHON) tools/export_kws_model.py $(TFLITE)
	@touch $@

$(MODEL_BIN) $(MODEL_ASM): $(MODEL_STAMP)

size: $(TARGET)
	@$(SIZE) $(TARGET)
	@echo ""
	@$(SIZE) -A $(TARGET) | awk '/^\.(text|rodata|data|bss|noncacheable|flower|kws_model|kikkar)/ {printf "  %-18s %9d B\n", $$1, $$2}'

mapcheck: $(TARGET)
	$(PYTHON) tools/check_memory_map.py

figures:
	$(PYTHON) tools/readme_figures.py

bin: $(TARGET)
	$(OBJCOPY) -O binary $(TARGET) $(BUILD)/$(PROJECT).bin

list: $(TARGET)
	$(OBJDUMP) -h -S $(TARGET) > $(BUILD)/$(PROJECT).list

clean:
	rm -rf $(OBJDIR) $(TARGET) $(BUILD)/$(PROJECT).map $(BUILD)/secure_nsclib.o

distclean: clean
	rm -f $(GENERATED) $(STAMP) $(MODEL_STAMP)

-include $(DEPS)

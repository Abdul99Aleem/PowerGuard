ARTIFACT = PowerGuard

#Build architecture/variant string, possible values: x86, armv7le, etc...
PLATFORM ?= aarch64le

#Build profile, possible values: release, debug, profile, coverage
BUILD_PROFILE ?= debug

CONFIG_NAME ?= $(PLATFORM)-$(BUILD_PROFILE)
OUTPUT_DIR = build/$(CONFIG_NAME)
TARGET = $(OUTPUT_DIR)/$(ARTIFACT)

#Compiler definitions

CC = qcc -Vgcc_nto$(PLATFORM)
CXX = q++ -Vgcc_nto$(PLATFORM)_cxx
LD = $(CC)

#User defined include/preprocessor flags and libraries

#INCLUDES += -I/path/to/my/lib/include
#INCLUDES += -I../mylib/public

#LIBS += -L/path/to/my/lib/$(PLATFORM)/usr/lib -lmylib
#LIBS += -L../mylib/$(OUTPUT_DIR) -lmylib

#Compiler flags for build profiles
CCFLAGS_release += -O2
CCFLAGS_debug += -g -O0 -fno-builtin
CCFLAGS_coverage += -g -O0 -ftest-coverage -fprofile-arcs
LDFLAGS_coverage += -ftest-coverage -fprofile-arcs
CCFLAGS_profile += -g -O0 -finstrument-functions
LIBS_profile += -lprofilingS

#Generic compiler flags (which include build type flags)
CCFLAGS_all += -Wall -fmessage-length=0
CCFLAGS_all += $(CCFLAGS_$(BUILD_PROFILE))
#Shared library has to be compiled with -fPIC
#CCFLAGS_all += -fPIC
LDFLAGS_all += $(LDFLAGS_$(BUILD_PROFILE))
LIBS_all += $(LIBS_$(BUILD_PROFILE))
DEPS = -Wp,-MMD,$(@:%.o=%.d),-MT,$@

# Each executable has its own entry point and shares the same policy and metrics.
.DEFAULT_GOAL := all
.PHONY: all clean rebuild
INCLUDES += -Iinclude -Itestbench
CCFLAGS_all += -std=gnu11

CORE_OBJS = $(OUTPUT_DIR)/supervisor/policy.o $(OUTPUT_DIR)/supervisor/metrics.o
SERVICE_OBJS = $(CORE_OBJS) $(OUTPUT_DIR)/src/PowerGuard.o
CONSOLE_OBJS = $(CORE_OBJS) $(OUTPUT_DIR)/tools/pg-console.o
CHECK_OBJS = $(CORE_OBJS) $(OUTPUT_DIR)/tests/replay.o $(OUTPUT_DIR)/tests/metric_checks.o
MANAGER_OBJS = $(OUTPUT_DIR)/manager/pg-manager.o
LED_OBJS = $(OUTPUT_DIR)/gpio/pg-state-leds.o
WATCHDOG_OBJS = $(OUTPUT_DIR)/watchdog/pg-watchdog.o
OBJS = $(sort $(SERVICE_OBJS) $(CONSOLE_OBJS) $(CHECK_OBJS) $(MANAGER_OBJS) $(LED_OBJS) $(WATCHDOG_OBJS))

all: $(TARGET) $(OUTPUT_DIR)/pg-console $(OUTPUT_DIR)/pg-check $(OUTPUT_DIR)/pg-manager $(OUTPUT_DIR)/pg-state-leds $(OUTPUT_DIR)/pg-watchdog

$(OUTPUT_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) -c $(DEPS) -o $@ $(INCLUDES) $(CCFLAGS_all) $(CCFLAGS) $<

$(TARGET): $(SERVICE_OBJS)
	$(LD) -o $@ $(LDFLAGS_all) $(LDFLAGS) $^ $(LIBS_all) $(LIBS)

$(OUTPUT_DIR)/pg-console: $(CONSOLE_OBJS)
	$(LD) -o $@ $(LDFLAGS_all) $(LDFLAGS) $^ $(LIBS_all) $(LIBS) -lm

$(OUTPUT_DIR)/pg-check: $(CHECK_OBJS)
	$(LD) -o $@ $(LDFLAGS_all) $(LDFLAGS) $^ $(LIBS_all) $(LIBS)

$(OUTPUT_DIR)/pg-manager: $(MANAGER_OBJS)
	$(LD) -o $@ $(LDFLAGS_all) $(LDFLAGS) $^ $(LIBS_all) $(LIBS)

$(OUTPUT_DIR)/pg-state-leds: $(LED_OBJS)
	$(LD) -o $@ $(LDFLAGS_all) $(LDFLAGS) $^ $(LIBS_all) $(LIBS)

$(OUTPUT_DIR)/pg-watchdog: $(WATCHDOG_OBJS)
	$(LD) -o $@ $(LDFLAGS_all) $(LDFLAGS) $^ $(LIBS_all) $(LIBS)

clean:
	rm -fr $(OUTPUT_DIR)

rebuild: clean all

#Inclusion of dependencies (object files to source and includes)
-include $(OBJS:%.o=%.d)

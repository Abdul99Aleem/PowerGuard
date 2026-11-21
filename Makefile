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
THREADCTL_OBJ = $(OUTPUT_DIR)/platform/threadctl.o
PROGRESS_OBJ = $(OUTPUT_DIR)/platform/progress.o
SERVICE_OBJS = $(CORE_OBJS) $(OUTPUT_DIR)/src/PowerGuard.o $(THREADCTL_OBJ) $(PROGRESS_OBJ)
CONSOLE_OBJS = $(CORE_OBJS) $(OUTPUT_DIR)/tools/pg-console.o
CHECK_OBJS = $(CORE_OBJS) $(OUTPUT_DIR)/tests/replay.o $(OUTPUT_DIR)/tests/metric_checks.o
MANAGER_OBJS = $(OUTPUT_DIR)/manager/pg-manager.o $(THREADCTL_OBJ)
LED_OBJS = $(OUTPUT_DIR)/gpio/pg-state-leds.o
RUNTIME_OBJS = $(OUTPUT_DIR)/runtime/runtime.o $(OUTPUT_DIR)/runtime/worker.o $(THREADCTL_OBJ) $(PROGRESS_OBJ)
THREADCTL_CHECK_OBJS = $(OUTPUT_DIR)/tests/threadctl_checks.o $(THREADCTL_OBJ)
VALIDATE_OBJS = $(OUTPUT_DIR)/tools/pg-validate.o
ENFORCER_OBJS = $(OUTPUT_DIR)/runtime/enforcer.o $(THREADCTL_OBJ)
STALL_CHECK_OBJS = $(OUTPUT_DIR)/tools/pg-stall-check.o $(THREADCTL_OBJ)
MANAGER_CHECK_OBJS = $(OUTPUT_DIR)/tests/manager_checks.o $(CORE_OBJS)
MERGE_CHECK_OBJS = $(OUTPUT_DIR)/tests/merge_checks.o $(CORE_OBJS)
SUPERVISOR_CHECK_OBJS = $(OUTPUT_DIR)/tools/pg-supervisor-check.o $(THREADCTL_OBJ) $(PROGRESS_OBJ)
OBJS = $(sort $(SERVICE_OBJS) $(CONSOLE_OBJS) $(CHECK_OBJS) $(MANAGER_OBJS) $(LED_OBJS) $(RUNTIME_OBJS) $(THREADCTL_CHECK_OBJS) $(VALIDATE_OBJS) $(ENFORCER_OBJS) $(STALL_CHECK_OBJS) $(MANAGER_CHECK_OBJS))
OBJS += $(MERGE_CHECK_OBJS) $(SUPERVISOR_CHECK_OBJS)

.PHONY: thermal-demo
thermal-demo: $(OUTPUT_DIR)/PowerGuard-thermal-demo $(OUTPUT_DIR)/pg-thermal-demo-check $(OUTPUT_DIR)/pg-demo-heater

$(OUTPUT_DIR)/pg-demo-heater: tools/pg-demo-heater.c $(THREADCTL_OBJ) include/pg_threadctl.h
	$(CC) $(INCLUDES) $(CCFLAGS_all) $(CCFLAGS) -o $@ tools/pg-demo-heater.c $(THREADCTL_OBJ) $(LDFLAGS_all) $(LDFLAGS) $(LIBS_all) $(LIBS) -pthread

$(OUTPUT_DIR)/PowerGuard-thermal-demo: src/PowerGuard.c supervisor/policy.c supervisor/metrics.c $(THREADCTL_OBJ) $(PROGRESS_OBJ) $(wildcard include/*.h)
	$(CC) $(INCLUDES) $(CCFLAGS_all) $(CCFLAGS) -DPG_THERMAL_DEMO -o $@ src/PowerGuard.c supervisor/policy.c supervisor/metrics.c $(THREADCTL_OBJ) $(PROGRESS_OBJ) $(LDFLAGS_all) $(LDFLAGS) $(LIBS_all) $(LIBS)

$(OUTPUT_DIR)/pg-thermal-demo-check: tests/thermal_demo_checks.c supervisor/policy.c $(wildcard include/*.h)
	@mkdir -p $(OUTPUT_DIR)
	$(CC) $(INCLUDES) $(CCFLAGS_all) $(CCFLAGS) -DPG_THERMAL_DEMO -o $@ supervisor/policy.c tests/thermal_demo_checks.c $(LDFLAGS_all) $(LDFLAGS) $(LIBS_all) $(LIBS)

all: $(TARGET) $(OUTPUT_DIR)/pg-console $(OUTPUT_DIR)/pg-check $(OUTPUT_DIR)/pg-manager $(OUTPUT_DIR)/pg-state-leds $(OUTPUT_DIR)/pg-thermal $(OUTPUT_DIR)/pg-runtime $(OUTPUT_DIR)/pg-threadctl-check $(OUTPUT_DIR)/pg-validate $(OUTPUT_DIR)/pg-enforcer $(OUTPUT_DIR)/pg-stall-check $(OUTPUT_DIR)/pg-manager-check $(OUTPUT_DIR)/pg-merge-check $(OUTPUT_DIR)/pg-supervisor-check

$(OUTPUT_DIR)/pg-supervisor-check: $(SUPERVISOR_CHECK_OBJS)
	$(LD) -o $@ $(LDFLAGS_all) $(LDFLAGS) $^ $(LIBS_all) $(LIBS)

$(OUTPUT_DIR)/pg-merge-check: $(MERGE_CHECK_OBJS)
	$(LD) -o $@ $(LDFLAGS_all) $(LDFLAGS) $^ $(LIBS_all) $(LIBS)

$(OUTPUT_DIR)/pg-enforcer: $(ENFORCER_OBJS)
	$(LD) -o $@ $(LDFLAGS_all) $(LDFLAGS) $^ $(LIBS_all) $(LIBS)

$(OUTPUT_DIR)/pg-stall-check: $(STALL_CHECK_OBJS)
	$(LD) -o $@ $(LDFLAGS_all) $(LDFLAGS) $^ $(LIBS_all) $(LIBS)

$(OUTPUT_DIR)/pg-manager-check: $(MANAGER_CHECK_OBJS)
	$(LD) -o $@ $(LDFLAGS_all) $(LDFLAGS) $^ $(LIBS_all) $(LIBS)

$(OUTPUT_DIR)/pg-validate: $(VALIDATE_OBJS)
	$(LD) -o $@ $(LDFLAGS_all) $(LDFLAGS) $^ $(LIBS_all) $(LIBS)

$(OUTPUT_DIR)/pg-runtime: $(RUNTIME_OBJS)
	$(LD) -o $@ $(LDFLAGS_all) $(LDFLAGS) $^ $(LIBS_all) $(LIBS) -pthread

$(OUTPUT_DIR)/pg-threadctl-check: $(THREADCTL_CHECK_OBJS)
	$(LD) -o $@ $(LDFLAGS_all) $(LDFLAGS) $^ $(LIBS_all) $(LIBS) -pthread

$(OUTPUT_DIR)/pg-thermal: $(OUTPUT_DIR)/telemetry/thermal.o $(OUTPUT_DIR)/supervisor/policy.o $(THREADCTL_OBJ)
	$(LD) -o $@ $(LDFLAGS_all) $(LDFLAGS) $^ $(LIBS_all) $(LIBS) -lm

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

clean:
	rm -fr $(OUTPUT_DIR)

rebuild: clean all

#Inclusion of dependencies (object files to source and includes)
-include $(OBJS:%.o=%.d)
-include $(OUTPUT_DIR)/telemetry/thermal.d

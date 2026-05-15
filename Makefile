CC = ppc-amigaos-gcc
STRIP = ppc-amigaos-strip
BUILD_DATE := $(shell date +"%d.%m.%Y")
BUILD_TIME := $(shell date +"%H:%M")
CFLAGS = -O2 -Wall -Wextra -Wshadow -Wformat=2 -I./include -fno-tree-loop-distribute-patterns \
         -DBUILD_DATE='"$(BUILD_DATE)"' -DBUILD_TIME='"$(BUILD_TIME)"'
DEPFLAGS = -MMD -MP
# -Wl,-z,common-page-size=4096 -Wl,-z,max-page-size=4096 reduces wasted
# zero-padding between sections from 64KB-aligned to 4KB-aligned (~28KB
# saved on the final binary).
LDFLAGS = -nostartfiles -Wl,-z,common-page-size=4096 -Wl,-z,max-page-size=4096

DOCKER_IMAGE = walkero/amigagccondocker:os4-gcc11
DOCKER_RUN   = docker run --rm -v "$(shell pwd):/work" -w /work $(DOCKER_IMAGE)

BUILD_DIR = build
DEBUG_OBJ_DIR = $(BUILD_DIR)/obj-debug
DIST_DIR = dist
DIST_NAME = VirtualSCSIDevice
TARGET = $(BUILD_DIR)/virtioscsi.device
TARGET_DEBUG = $(BUILD_DIR)/virtioscsi.device.debug
SRC = src/device.c src/Init.c src/Open.c src/Close.c src/Expunge.c src/BeginIO.c \
      src/scsi_cdb_helpers.c src/cmd_names.c src/unit_discovery.c src/unit_task.c \
      src/exec_cmds/cmd_read.c src/exec_cmds/cmd_write.c \
      src/exec_cmds/cmd_stubs.c src/exec_cmds/cmd_td_getgeometry.c \
      src/exec_cmds/cmd_td_getnumtracks.c \
      src/exec_cmds/cmd_update.c src/exec_cmds/cmd_td_io64.c \
      src/scsi_cmds/scsi_parse.c src/scsi_cmds/scsi_inquiry.c \
      src/scsi_cmds/scsi_read_capacity_10.c src/scsi_cmds/scsi_rw_10.c \
      src/scsi_cmds/scsi_test_unit_ready.c \
      src/scsi_cmds/scsi_log_sense.c \
      src/scsi_cmds/scsi_ata_passthrough.c \
      src/ns_cmds/ns_parse.c src/ns_cmds/ns_devicequery.c \
      src/ns_cmds/ns_td_getgeometry64.c src/ns_cmds/ns_td_io64.c \
      src/pci/pci_discovery.c src/pci/pci_modern_detect.c \
      src/virtio/virtio_init.c src/virtio/virtqueue.c \
      src/virtio/virtio_irq.c src/virtio/virtio_scsi_io.c

OBJ = $(patsubst src/%.c, $(BUILD_DIR)/%.o, $(SRC))
OBJ_DEBUG = $(patsubst src/%.c, $(DEBUG_OBJ_DIR)/%.o, $(SRC))
DEP = $(OBJ:.o=.d) $(OBJ_DEBUG:.o=.d)

.PHONY: all dist dist-lha clean help

all: $(BUILD_DIR) $(TARGET) $(TARGET_DEBUG) $(BUILD_DIR)/test_virtioscsi $(BUILD_DIR)/test_modern $(BUILD_DIR)/test_inquiry

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

# Release: compile without -DDEBUG, strip symbols.  This is what goes into
# SYS:Kickstart/ on end-user installs.
$(TARGET): $(OBJ)
	$(CC) $(OBJ) -o $(TARGET) $(LDFLAGS)
	@echo "Stripping release build..."
	$(STRIP) --strip-all $(TARGET)

# Debug: recompile every source with -DDEBUG so DPRINTF() emits via
# DebugPrintF and do NOT strip, so stack traces decode.  Ships alongside
# the release binary in the LHA for diagnostic sessions.
$(TARGET_DEBUG): $(OBJ_DEBUG)
	$(CC) $(OBJ_DEBUG) -o $(TARGET_DEBUG) $(LDFLAGS)
	@echo "Debug build produced (DPRINTF active, symbols kept)"

$(BUILD_DIR)/test_virtioscsi: tests/test_virtioscsi.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) $< -o $@ -lauto

$(BUILD_DIR)/test_modern: tests/test_modern.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) $< -o $@ -lauto

$(BUILD_DIR)/test_inquiry: tests/test_inquiry.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) $< -o $@ -lauto

$(BUILD_DIR)/%.o: src/%.c | $(BUILD_DIR)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(DEPFLAGS) -c $< -o $@

$(DEBUG_OBJ_DIR)/%.o: src/%.c | $(BUILD_DIR)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -DDEBUG $(DEPFLAGS) -c $< -o $@

-include $(DEP)

# Create distribution directory with all files needed to run on AmigaOS 4
dist: all
	@echo "=== Creating distribution ==="
	rm -rf $(DIST_DIR)/$(DIST_NAME)
	mkdir -p $(DIST_DIR)/$(DIST_NAME)
	@# Device driver (stripped release + unstripped debug variant)
	cp $(BUILD_DIR)/virtioscsi.device $(DIST_DIR)/$(DIST_NAME)/
	-cp $(BUILD_DIR)/virtioscsi.device.debug $(DIST_DIR)/$(DIST_NAME)/ 2>/dev/null || true
	@# Install script
	cp Autoinstall                    $(DIST_DIR)/$(DIST_NAME)/
	@# Documentation
	cp README_os4depot.txt            $(DIST_DIR)/$(DIST_NAME)/
	@echo "Distribution created in $(DIST_DIR)/$(DIST_NAME)/"
	@echo "Contents:"
	@find $(DIST_DIR)/$(DIST_NAME) -type f | sort

# Create LHA archive from distribution
dist-lha: dist
	@echo "=== Creating LHA archive ==="
	$(DOCKER_RUN) sh -c 'cd $(DIST_DIR) && lha ao5q /work/$(DIST_DIR)/$(DIST_NAME).lha $(DIST_NAME)'
	@ls -la $(DIST_DIR)/$(DIST_NAME).lha
	@echo "Archive created: $(DIST_DIR)/$(DIST_NAME).lha"

clean:
	rm -rf $(BUILD_DIR) $(DIST_DIR)

help:
	@echo "virtioscsi.device Build System"
	@echo ""
	@echo "Targets:"
	@echo "  all       - Build device driver and test programs (default)"
	@echo "  dist      - Create distribution directory for AmigaOS 4"
	@echo "  dist-lha  - Create LHA archive ($(DIST_DIR)/$(DIST_NAME).lha)"
	@echo "  clean     - Remove all build artifacts"
	@echo "  help      - Show this help"
	@echo ""
	@echo "Build via Docker:"
	@echo "  docker run --rm -v \$$(pwd):/src -w /src $(DOCKER_IMAGE) make"
	@echo "  docker run --rm -v \$$(pwd):/src -w /src $(DOCKER_IMAGE) make dist-lha"
	@echo ""
	@echo "Debug build:"
	@echo "  make CFLAGS=\"-O2 -Wall -Wextra -I./include -fno-tree-loop-distribute-patterns -DDEBUG\""

CC = ppc-amigaos-gcc
CFLAGS = -O2 -Wall -I./include -fno-tree-loop-distribute-patterns
LDFLAGS = -nostartfiles

BUILD_DIR = build
DIST_DIR = dist
TARGET = $(BUILD_DIR)/virtioscsi.device
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
      src/pci/pci_discovery.c src/virtio/virtio_init.c src/virtio/virtqueue.c \
      src/virtio/virtio_irq.c src/virtio/virtio_scsi_io.c

OBJ = $(patsubst src/%.c, $(BUILD_DIR)/%.o, $(SRC))

all: $(BUILD_DIR) $(TARGET) $(BUILD_DIR)/test_virtioscsi

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(TARGET): $(OBJ)
	$(CC) $(OBJ) -o $(TARGET) $(LDFLAGS)

$(BUILD_DIR)/test_virtioscsi: tests/test_virtioscsi.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) $< -o $@ -lauto

$(BUILD_DIR)/%.o: src/%.c | $(BUILD_DIR)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -rf $(BUILD_DIR) $(DIST_DIR)

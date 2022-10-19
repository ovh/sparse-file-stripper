.DEFAULT_GOAL := all

BUILD_DIR := build
BIN_DIR := $(BUILD_DIR)/bin
SRC_DIR := src
CC := gcc
CFLAGS := -I$(SRC_DIR)/include -Wall
DEBUG ?= 0
ifeq ($(DEBUG), 1)
	CFLAGS += -DDEBUG -g
endif
SRC := $(wildcard $(SRC_DIR)/*.c)
OBJS := $(SRC:$(SRC_DIR)/%.c=$(BUILD_DIR)/%.o)
# alternative: OBJS := $(patsubst $(SRC_DIR)/%.c, $(BUILD_DIR)/%.o, $(SRC))
ODEPS := $(addprefix $(BUILD_DIR)/, common.o)
BINS := sfsz sfsuz sfs_stats

.PHONY: clean all
.SECONDEXPANSION: $(BINS)

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c | $(BUILD_DIR)
	$(CC) -c -o $@ $< $(CFLAGS)

$(BINS): $(ODEPS) $(BUILD_DIR)/$$@.o | $(BIN_DIR)
	$(CC) -o $(BIN_DIR)/$@ $^ $(CFLAGS)
# alternative without secondary expansion
#$(BINS): $(OBJS) | $(BIN_DIR)
#        $(CC) -o $(BIN_DIR)/$@ $(ODEPS) $@.o $(CFLAGS)


$(BUILD_DIR) $(BIN_DIR):
	mkdir -p $@

clean:
	rm -rf $(BUILD_DIR)

all: $(BINS)

fresh: clean all

# Secure Bank - build system
CC      ?= gcc
CFLAGS  ?= -std=c11 -O2 -g
WARN    := -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wstrict-prototypes -Wformat=2
INC     := -Iinclude

SRC_LIB := src/sha256.c src/auth.c src/utils.c src/bank.c src/storage.c src/ui.c
OBJ_DIR := build
LIB_OBJ := $(SRC_LIB:src/%.c=$(OBJ_DIR)/%.o)
MAIN_OBJ:= $(OBJ_DIR)/main.o
TARGET  := bank
TEST_BIN:= $(OBJ_DIR)/test_bank

.PHONY: all test run demo debug clean

all: $(TARGET)

$(TARGET): $(LIB_OBJ) $(MAIN_OBJ)
	$(CC) $(CFLAGS) -o $@ $^

$(OBJ_DIR)/%.o: src/%.c | $(OBJ_DIR)
	$(CC) $(CFLAGS) $(WARN) $(INC) -MMD -MP -c $< -o $@

$(OBJ_DIR):
	mkdir -p $(OBJ_DIR)

$(TEST_BIN): tests/test_bank.c $(filter-out $(OBJ_DIR)/ui.o,$(LIB_OBJ))
	$(CC) $(CFLAGS) $(WARN) $(INC) -o $@ $^

test: $(TEST_BIN)
	./$(TEST_BIN)

run: $(TARGET)
	./$(TARGET)

demo: $(TARGET)
	./$(TARGET) --data-dir demo_data --demo

# Build + test with AddressSanitizer and UBSan
debug:
	$(MAKE) clean
	$(MAKE) test CFLAGS="-std=c11 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer"

clean:
	rm -rf $(OBJ_DIR) $(TARGET) $(TARGET).exe

-include $(LIB_OBJ:.o=.d) $(MAIN_OBJ:.o=.d)

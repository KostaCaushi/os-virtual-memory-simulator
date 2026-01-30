CC = gcc
CFLAGS = -Wall -Wextra -g

TARGET = ossim
SRC = src/main.c
BUILD = build

all: $(TARGET)

$(TARGET): $(SRC) | $(BUILD)
	$(CC) $(CFLAGS) $(SRC) -o $(TARGET)

$(BUILD):
	mkdir -p $(BUILD)

clean:
	rm -rf $(BUILD) $(TARGET)
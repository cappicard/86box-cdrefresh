CC      = gcc
CFLAGS  = -Wall -Wextra -O2
TARGET  = detect-cd

all: $(TARGET)

clean:
	rm -f $(TARGET)

$(TARGET): detect-cd.c
	$(CC) $(CFLAGS) -o $(TARGET) detect-cd.c

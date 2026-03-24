CC = gcc
CFLAGS = -Wall -Wextra
TARGET = OWR
SRCS = main.c network.c routing.c neighbors.c commands.c messages.c

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRCS)

clean:
	rm -f $(TARGET)
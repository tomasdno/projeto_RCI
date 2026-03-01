CC = gcc
CFLAGS = -Wall -Wextra -std=c11 -g

OBJ = tourists.o map.o tasks.o quests.o
TARGET = tourists

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $(CFLAGS) -o $(TARGET) $(OBJ)

tourists.o: tourists.c map.h tasks.h quests.h
	$(CC) $(CFLAGS) -c tourists.c

map.o: map.c map.h
	$(CC) $(CFLAGS) -c map.c

tasks.o: tasks.c tasks.h map.h
	$(CC) $(CFLAGS) -c tasks.c

quests.o: quests.c quests.h
	$(CC) $(CFLAGS) -c quests.c

clean:
	rm -f $(OBJ) $(TARGET)
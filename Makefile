CC = gcc
CFLAGS = -O2 -std=c11 -Wall -Wextra
TARGET_CONSOLE = student_system
TARGET_WEB = student_system_web
SRC = student_system.c
WEB_SRC = student_system_web.c

all: $(TARGET_CONSOLE) $(TARGET_WEB)

$(TARGET_CONSOLE): $(SRC)
	$(CC) $(CFLAGS) -o $@ $^

$(TARGET_WEB): $(SRC) $(WEB_SRC)
	$(CC) $(CFLAGS) -DBUILD_WEB -o $@ $(SRC) $(WEB_SRC)

clean:
	rm -f $(TARGET_CONSOLE) $(TARGET_WEB)

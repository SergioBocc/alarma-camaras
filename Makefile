# ============================================================
# Makefile — alarm_processor
# ============================================================
 
TARGET   = alarm_processor
CC       = gcc
CFLAGS   = -Wall -Wextra -O2 -pthread \
           -I. \
           -Iinih \
           -Ithird_party
 
ORT_DIR  = ./onnxruntime
CFLAGS  += -I$(ORT_DIR)/include
LDFLAGS  = -L$(ORT_DIR)/lib -lonnxruntime
LDFLAGS += -lcurl
LDFLAGS += -lgpiod
LDFLAGS += -lmosquitto
LDFLAGS += -lssl -lcrypto
LDFLAGS += -lm
LDFLAGS += -lpthread
 
SRCS     = main.c \
           config.c \
           log.c \
           imap.c \
           yolo.c \
           gpio.c \
           burst.c \
           mqtt_alarm.c \
	   detlog.c \
           inih/ini.c
 
OBJS     = $(SRCS:.c=.o)
 
.PHONY: all clean install
 
all: $(TARGET)
 
$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)
	@echo ""
	@echo "  ✓ Compilado: ./$(TARGET)"
	@echo ""
 
%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<
 
clean:
	rm -f $(OBJS) $(TARGET)
 
install: $(TARGET)
	sudo cp $(TARGET) /usr/local/bin/
	sudo cp alarm_processor.service /etc/systemd/system/
	sudo systemctl daemon-reload
	sudo systemctl enable alarm_processor
	@echo "Servicio instalado. Iniciar con: sudo systemctl start alarm_processor"
 

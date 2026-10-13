# Testes do pipeline de DSP executados no computador (gcc).
CFLAGS = -std=c11 -O2 -Wall -Wextra -D_DEFAULT_SOURCE -I../../include -I../../include/app
SRC = ../../app/dsp.c ../../app/note_tracker.c

all: run monitor

test_pipeline: test_pipeline.c $(SRC)
	$(CC) $(CFLAGS) -o $@ test_pipeline.c $(SRC) -lm

run: test_pipeline
	./test_pipeline

# Monitor da execucao no terminal serial
test_monitor: test_monitor.c ../../app/execution_monitor.c ../../include/app/execution_monitor.h
	$(CC) $(CFLAGS) -o $@ test_monitor.c ../../app/execution_monitor.c -lm

monitor: test_monitor
	./test_monitor

# Dispositivo falso (UDP) para testar o app sem hardware
fake_device: fake_device.c ../../include/protocol.h
	$(CC) -std=gnu11 -O2 -Wall -Wextra -I../../include -o $@ fake_device.c

clean:
	rm -f test_pipeline test_monitor fake_device

# Testes do pipeline de DSP executados no computador (gcc).
CFLAGS = -std=c11 -O2 -Wall -Wextra -D_DEFAULT_SOURCE
SRC = ../../src/dsp.c ../../src/note_tracker.c

all: run

test_pipeline: test_pipeline.c $(SRC)
	$(CC) $(CFLAGS) -o $@ test_pipeline.c $(SRC) -lm

run: test_pipeline
	./test_pipeline

clean:
	rm -f test_pipeline

CC      = gcc
CFLAGS  = -Wall -Wextra -D_GNU_SOURCE -Iinclude -fPIC -pthread -g -O2
LDFLAGS = -shared -ldl -lpthread

SRCS    = src/core.c src/interceptor.c src/lock_graph.c src/filters.c \
          src/wait_graph.c src/watchdog.c src/stress.c src/report.c
OBJS    = $(SRCS:.c=.o)
TARGET  = libdeadlockscope.so

TEST_SRCS = $(wildcard tests/[0-9]*.c)
TEST_BINS = $(patsubst tests/%.c, tests/bin/%, $(TEST_SRCS))

all: $(TARGET) tests

$(TARGET): $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $(OBJS)

src/%.o: src/%.c
	$(CC) $(CFLAGS) -c $< -o $@

tests: $(TARGET) $(TEST_BINS)

tests/bin/%: tests/%.c
	@mkdir -p tests/bin
	$(CC) -Wall -Wextra -pthread -g -O0 -o $@ $<

clean:
	rm -f src/*.o $(TARGET) tests/bin/*

check: all
	@chmod +x tests/run_tests.sh cli/deadlock-scope
	@./tests/run_tests.sh

compare: all
	@chmod +x tests/compare/run_comparison.sh cli/deadlock-scope
	@./tests/compare/run_comparison.sh

.PHONY: all clean tests check compare

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
	$(CC) -Wall -Wextra -pthread -rdynamic -g -O0 -o $@ $<

clean:
	rm -f src/*.o $(TARGET) tests/bin/*

check: all
	@chmod +x tests/run_tests.sh cli/deadlock-scope
	@./tests/run_tests.sh

compare: all
	@chmod +x tests/compare/run_comparison.sh cli/deadlock-scope
	@./tests/compare/run_comparison.sh

html: all
	@mkdir -p reports
	@echo "Generating full suite of interactive HTML diagnostic reports..."
	@timeout 2 ./cli/deadlock-scope --html reports/01_simple_deadlock.html -- ./tests/bin/01_simple_deadlock >/dev/null 2>&1 || true
	@./cli/deadlock-scope --html reports/04_gate_lock.html -- ./tests/bin/04_gate_lock >/dev/null 2>&1 || true
	@./cli/deadlock-scope --html reports/05_same_thread.html -- ./tests/bin/05_same_thread >/dev/null 2>&1 || true
	@./cli/deadlock-scope --html reports/06_lifecycle_ordered.html -- ./tests/bin/06_lifecycle_ordered >/dev/null 2>&1 || true
	@timeout 2 ./cli/deadlock-scope --stress 50 --html reports/08_provocation_test.html -- ./tests/bin/08_provocation_test >/dev/null 2>&1 || true
	@./cli/deadlock-scope --html reports/10_clean_parallel.html -- ./tests/bin/10_clean_parallel >/dev/null 2>&1 || true
	@timeout 2 ./cli/deadlock-scope --html reports/12_timedlock.html -- ./tests/bin/12_timedlock_deadlock >/dev/null 2>&1 || true
	@echo "Diagnostic HTML reports generated in reports/:"
	@ls -lh reports/*.html

.PHONY: all clean tests check compare html

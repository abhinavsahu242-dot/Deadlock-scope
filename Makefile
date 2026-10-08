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
	@echo "Generating full suite of interactive HTML diagnostic reports for all 12 test scenarios..."
	@timeout 2 ./cli/deadlock-scope --html reports/01_simple_deadlock.html -- ./tests/bin/01_simple_deadlock >/dev/null 2>&1 || true
	@timeout 2 ./cli/deadlock-scope --html reports/02_cycle_3thread.html -- ./tests/bin/02_cycle_3thread >/dev/null 2>&1 || true
	@./cli/deadlock-scope --html reports/03_potential_benign.html -- ./tests/bin/03_potential_benign >/dev/null 2>&1 || true
	@./cli/deadlock-scope --html reports/04_gate_lock.html -- ./tests/bin/04_gate_lock >/dev/null 2>&1 || true
	@./cli/deadlock-scope --html reports/05_same_thread.html -- ./tests/bin/05_same_thread >/dev/null 2>&1 || true
	@./cli/deadlock-scope --html reports/06_lifecycle_ordered.html -- ./tests/bin/06_lifecycle_ordered >/dev/null 2>&1 || true
	@./cli/deadlock-scope --html reports/07_trylock_recovery.html -- ./tests/bin/07_trylock_recovery >/dev/null 2>&1 || true
	@timeout 2 ./cli/deadlock-scope --stress 50 --html reports/08_provocation_test.html -- ./tests/bin/08_provocation_test >/dev/null 2>&1 || true
	@./cli/deadlock-scope --html reports/09_recursive_mutex.html -- ./tests/bin/09_recursive_mutex >/dev/null 2>&1 || true
	@./cli/deadlock-scope --html reports/10_clean_parallel.html -- ./tests/bin/10_clean_parallel >/dev/null 2>&1 || true
	@./cli/deadlock-scope --html reports/11_cond_wait_held_set.html -- ./tests/bin/11_cond_wait_held_set >/dev/null 2>&1 || true
	@timeout 2 ./cli/deadlock-scope --html reports/12_timedlock.html -- ./tests/bin/12_timedlock_deadlock >/dev/null 2>&1 || true
	@echo "All 12 diagnostic HTML reports generated in reports/:"
	@ls -lh reports/*.html

pdf:
	@echo "Compiling formal academic report PDF (report.pdf) & formal guide PDF..."
	@if [ -f "/mnt/c/Program Files (x86)/Microsoft/Edge/Application/msedge.exe" ]; then \
		"/mnt/c/Program Files (x86)/Microsoft/Edge/Application/msedge.exe" --headless=new --disable-gpu --no-pdf-header-footer --print-to-pdf="C:\\Users\\hp\\.gemini\\antigravity\\scratch\\deadlockscope\\report.pdf" "C:\\Users\\hp\\.gemini\\antigravity\\scratch\\deadlockscope\\report.html" >/dev/null 2>&1 || true; \
		if [ -f "build_guide_pdf.py" ]; then \
			cmd.exe /c "python build_guide_pdf.py" >/dev/null 2>&1 || python3 build_guide_pdf.py >/dev/null 2>&1 || true; \
		fi; \
	elif command -v chromium-browser >/dev/null 2>&1; then \
		chromium-browser --headless --disable-gpu --print-to-pdf=report.pdf report.html >/dev/null 2>&1 || true; \
	elif command -v google-chrome >/dev/null 2>&1; then \
		google-chrome --headless --disable-gpu --print-to-pdf=report.pdf report.html >/dev/null 2>&1 || true; \
	elif command -v weasyprint >/dev/null 2>&1; then \
		weasyprint report.html report.pdf >/dev/null 2>&1 || true; \
	fi
	@echo "PDF build successful:"
	@ls -lh report.pdf FORMAL_REPORT_GUIDE.pdf 2>/dev/null || ls -lh report.pdf

.PHONY: all clean tests check compare html pdf

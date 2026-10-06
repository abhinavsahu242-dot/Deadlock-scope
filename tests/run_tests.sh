#!/usr/bin/env bash
# DeadlockScope Automated Test Harness

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN_DIR="${SCRIPT_DIR}/tests/bin"
CLI="${SCRIPT_DIR}/cli/deadlock-scope"

chmod +x "$CLI"

TOTAL_TESTS=0
PASSED_TESTS=0

run_test() {
    local name="$1"
    local bin="$2"
    local cli_flags="$3"
    local must_contain="$4"
    local must_not_contain="$5"
    local timeout_sec="${6:-2}"

    TOTAL_TESTS=$((TOTAL_TESTS + 1))
    echo -n "[$TOTAL_TESTS] Running $name ... "

    local output_file
    output_file=$(mktemp)

    # Run with timeout to catch hanging tests
    timeout "$timeout_sec" "$CLI" $cli_flags -- "$bin" >"$output_file" 2>&1
    local exit_code=$?

    local fail=0
    local reason=""

    if [[ -n "$must_contain" ]]; then
        if ! grep -q "$must_contain" "$output_file"; then
            fail=1
            reason="Missing expected pattern: '$must_contain'"
        fi
    fi

    if [[ -n "$must_not_contain" && $fail -eq 0 ]]; then
        if grep -q "$must_not_contain" "$output_file"; then
            fail=1
            reason="Unexpected pattern found: '$must_not_contain'"
        fi
    fi

    if [[ $fail -eq 0 ]]; then
        echo -e "\e[32mPASS\e[0m"
        PASSED_TESTS=$((PASSED_TESTS + 1))
    else
        echo -e "\e[31mFAIL\e[0m ($reason)"
        if [[ "${DEBUG_FAIL:-1}" -eq 1 ]]; then
            echo "--- Output for $name ---"
            cat "$output_file"
            echo "------------------------"
        fi
    fi
    rm -f "$output_file"
}

echo "================================================================================"
echo "                   DEADLOCKSCOPE AUTOMATED VERIFICATION SUITE                   "
echo "================================================================================"

run_test "01_simple_deadlock (Live 2-Thread)" \
         "${BIN_DIR}/01_simple_deadlock" "" "DEADLOCK DETECTED BY WATCHDOG" "" 2

run_test "02_cycle_3thread (Live 3-Thread)" \
         "${BIN_DIR}/02_cycle_3thread" "" "DEADLOCK DETECTED BY WATCHDOG" "" 2

run_test "03_potential_benign (Prediction)" \
         "${BIN_DIR}/03_potential_benign" "" "POTENTIAL DEADLOCK PREDICTED" "ACTIVE DEADLOCK DETECTED" 2

run_test "04_gate_lock (Gate-Lock Filter)" \
         "${BIN_DIR}/04_gate_lock" "--debug" "gate_lock" "POTENTIAL DEADLOCK PREDICTED" 2

run_test "05_same_thread (Same-Thread Filter)" \
         "${BIN_DIR}/05_same_thread" "--debug" "same_thread" "POTENTIAL DEADLOCK PREDICTED" 2

run_test "06_lifecycle_ordered (Lifecycle Filter)" \
         "${BIN_DIR}/06_lifecycle_ordered" "--debug" "lifecycle" "POTENTIAL DEADLOCK PREDICTED" 2

run_test "07_trylock_recovery (Trylock Clean)" \
         "${BIN_DIR}/07_trylock_recovery" "" "without deadlock" "POTENTIAL DEADLOCK PREDICTED" 2

run_test "08_provocation_test [Unstressed]" \
         "${BIN_DIR}/08_provocation_test" "" "POTENTIAL DEADLOCK PREDICTED" "DEADLOCK DETECTED" 2

run_test "08_provocation_test [Stressed]" \
         "${BIN_DIR}/08_provocation_test" "--stress 60" "DEADLOCK DETECTED BY WATCHDOG" "" 2

run_test "09_recursive_mutex (Re-entrancy)" \
         "${BIN_DIR}/09_recursive_mutex" "" "Recursive mutex test passed cleanly" "POTENTIAL DEADLOCK PREDICTED" 2

run_test "10_clean_parallel (Scalability & Clean)" \
         "${BIN_DIR}/10_clean_parallel" "" "Clean parallel test completed with zero violations" "POTENTIAL DEADLOCK PREDICTED" 2

run_test "11_cond_wait_held_set (Condvar Safety)" \
         "${BIN_DIR}/11_cond_wait_held_set" "" "Condvar held-set test completed cleanly" "POTENTIAL DEADLOCK PREDICTED" 2

run_test "12_timedlock_deadlock (ACTIVE_TIMED)" \
         "${BIN_DIR}/12_timedlock_deadlock" "" "ACTIVE_TIMED" "" 2

echo "--------------------------------------------------------------------------------"
echo "Results: $PASSED_TESTS / $TOTAL_TESTS passed."
echo "================================================================================"

echo ""
echo "================================================================================"
echo "               FILTER ABLATION STUDY: DEADLOCKSCOPE VS LOCKDEP                  "
echo "================================================================================"
printf "%-30s | %-20s | %-20s\n" "Test Case" "L0 (Raw lockdep: none)" "L3 (DeadlockScope: all)"
echo "--------------------------------------------------------------------------------"

for t in "04_gate_lock" "05_same_thread" "06_lifecycle_ordered"; do
    raw_res=$("$CLI" --filters none -- "${BIN_DIR}/$t" 2>&1 | grep -c "POTENTIAL DEADLOCK PREDICTED")
    filt_res=$("$CLI" --filters all -- "${BIN_DIR}/$t" 2>&1 | grep -c "POTENTIAL DEADLOCK PREDICTED")
    
    r_label="POTENTIAL (False Alarm)"
    f_label="SUPPRESSED (Accurate)"
    if [[ "$raw_res" -eq 0 ]]; then r_label="CLEAN"; fi
    if [[ "$filt_res" -ne 0 ]]; then f_label="POTENTIAL"; fi

    printf "%-30s | %-20s | %-20s\n" "$t" "$r_label" "$f_label"
done
echo "================================================================================"

if [[ $PASSED_TESTS -eq $TOTAL_TESTS ]]; then
    exit 0
else
    exit 1
fi

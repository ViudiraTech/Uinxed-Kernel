#!/usr/bin/env python3
"""Compile the actual snapshot publisher with a counter that advances between reads.

--baseline REF uses the publisher at that Git revision and expects an anchor mismatch.
"""
import argparse
import pathlib
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]


def function(source, name):
    start = source.index(name + "(")
    start = source.rfind("\n", 0, start) + 1
    brace = source.index("{", start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end] + "\n"


prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#define INCLUDE_STDINT_H_
#include <kernel/vdso/vdso.h>
static uint64_t tsc_frequency = 2000000000ULL, tsc_epoch_value = 100;
static uint64_t tsc_epoch_ns = 500, tsc_ns_ratio = 1ULL << 31;
static uint64_t timer_monotonic_floor_ns, counter = 4000000000000ULL;
static int64_t timer_realtime_base_ns = 1700000000000000000LL;
static struct vdso_data storage, *vdso_page = &storage;
static uint64_t vdso_ns_per_sec = 2000000000ULL;
/* Simulate a 5 ms scheduling delay after every hardware read. */
uint64_t rdtsc_serialized(void) { uint64_t sampled = counter; counter += 10000000; return sampled; }
int tsc_clocksource_available(void) { return 1; }
int hpet_available(void) { return 0; }
uint64_t nano_time(void) { return 0; }
uint64_t sched_ticks(void) { return 0; }
uint64_t timer_ticks_to_ns(uint64_t ticks) { return ticks * 1000000; }
'''
code = prefix
code += function((ROOT / "drivers/time/tsc.c").read_text(), "tsc_nano_time_at")
timer = (ROOT / "kernel/timer/timer.c").read_text()
for name in ("timer_monotonic_sample", "timer_monotonic_ns", "timer_realtime_from_monotonic_ns", "timer_realtime_ns"):
    code += function(timer, name)
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--baseline", metavar="REF", help="Git revision of the pre-fix publisher")
args = parser.parse_args()
baseline = args.baseline is not None
publisher = subprocess.check_output(["git", "show", args.baseline + ":kernel/vdso/vdso.c"], cwd=ROOT, text=True) if baseline else (ROOT / "kernel/vdso/vdso.c").read_text()
code += function(publisher, "vdso_publish")
code += r'''
int main(void) {
    vdso_publish();
    uint64_t published = storage.mono_sec * 1000000000ULL + storage.mono_nsec;
    uint64_t anchored = tsc_nano_time_at(storage.cycle_last);
    printf("VDSO_SNAPSHOT published=%llu anchor_time=%llu mismatch_ns=%lld\n",
           (unsigned long long)published, (unsigned long long)anchored,
           (long long)(anchored - published));
    if (published != anchored) return 2;
    assert(!(storage.seq & 1));
    assert(storage.real_sec * 1000000000ULL + storage.real_nsec == published + timer_realtime_base_ns);
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix="vdso-snapshot-") as tmp:
    source = pathlib.Path(tmp) / "fixture.c"
    binary = pathlib.Path(tmp) / "fixture"
    source.write_text(code)
    subprocess.run(["cc", "-O2", "-Wall", "-Wextra", "-Werror", "-fsanitize=undefined", "-I", str(ROOT / "include"), str(source), "-o", str(binary)], check=True)
    result = subprocess.run([str(binary)])
    expected = 2 if baseline else 0
    if result.returncode != expected:
        raise SystemExit(f"Unexpected result: got {result.returncode}, expected {expected}")
    print("Baseline mismatch reproduced" if baseline else "Snapshot anchor regression passed")

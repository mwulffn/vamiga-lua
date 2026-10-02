#!/usr/bin/env python3
"""Run the test modules in this directory in parallel.

Usage: run_tests.py [--jobs N] [module ...]

Each module is run by its own "python -m unittest" process, N at a time
(default: half the number of CPU cores), each with its own emulators. Without module names, all
test_*.py files are run. The environment variables described in harness.py
apply. The exit status is 1 if a test failed.
"""

import argparse
import os
import re
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

DIRECTORY = Path(__file__).resolve().parent


def run_module(module: str) -> tuple[str, int, str, float]:
    """Run a test module. Return its name, exit status, output and time."""
    start = time.monotonic()
    process = subprocess.run(
        [sys.executable, "-m", "unittest", module],
        cwd=DIRECTORY,
        capture_output=True,
        text=True,
    )
    return module, process.returncode, process.stderr + process.stdout, time.monotonic() - start


def main() -> int:
    parser = argparse.ArgumentParser(description="Run the Lua tests in parallel")
    # An emulator keeps two threads busy.
    default_jobs = max(2, (os.cpu_count() or 4) // 2)
    parser.add_argument(
        "--jobs", type=int, default=default_jobs, help="modules to run at the same time"
    )
    parser.add_argument("modules", nargs="*", help="test modules (default: all)")
    args = parser.parse_args()
    modules = args.modules or sorted(path.stem for path in DIRECTORY.glob("test_*.py"))

    start = time.monotonic()
    tests = 0
    failed = []
    with ThreadPoolExecutor(max_workers=args.jobs) as executor:
        for module, status, output, seconds in executor.map(run_module, modules):
            # The last lines of unittest are "Ran N tests in ..." and the result.
            ran = re.search(r"^Ran (\d+) tests?", output, re.MULTILINE)
            tests += int(ran.group(1)) if ran else 0
            result = output.strip().splitlines()[-1] if output.strip() else "no output"
            print(f"{module:20} {seconds:5.1f} s  {result}")
            if status != 0:
                failed.append((module, output))

    for module, output in failed:
        print(f"\n{'=' * 70}\n{module}\n{'=' * 70}\n{output}")
    seconds = time.monotonic() - start
    print(f"\n{tests} tests in {seconds:.1f} s, {len(failed)} of {len(modules)} modules failed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())

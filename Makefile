# Host tests. `make` builds and runs everything that does not need ESP-IDF.
# The firmware itself is cross-built in CI (.github/workflows/ci.yml).

.PHONY: all run asan clean

all: run

run:
	./tests/run.sh

asan:
	./tests/run.sh --asan

tools:
	python3 tests/test_tools.py

clean:
	rm -rf /tmp/frame_host_tests build build-release dist
	find . -name __pycache__ -type d -prune -exec rm -rf {} +

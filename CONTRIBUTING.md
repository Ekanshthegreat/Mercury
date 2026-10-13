# Development

Build with C++20 and strict warnings; run CTest and `scripts/check_cli.py` before
submitting changes. Use `cmake --preset sanitize` for memory/undefined-behavior
checks. New policy logic should depend only on `Observation`, with prefix tests
showing that changes to future events do not change earlier decisions.

Keep result formats explicit and versioned. Generated charts must come from
exported results. Document changes to seed handling, matching behavior and cost
accounting, because they can invalidate comparisons with stored examples.

`clang-format` follows the repository configuration. The simulator and matching
engine are independent of wall time; timing belongs in benchmarks only.

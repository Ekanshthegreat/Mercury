cmake_minimum_required(VERSION 3.20)

# Benchmarks are plain executables with hand-rolled timing so the project keeps
# zero external dependencies. Each prints machine-readable lines
# ("bench,<name>,<metric>,<value>,<unit>") that scripts/ can collect, and each
# records the hypothesis it is testing at the top of the source.

add_executable(bench_submit_latency bench_submit_latency.cpp)
target_link_libraries(bench_submit_latency PRIVATE mercury_engine)

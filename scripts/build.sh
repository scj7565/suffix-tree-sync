#!/bin/bash
# tree_bench 빌드 (Linux: WSL2/Colab). 결과: bin/tree_bench
# ThreadSanitizer(경쟁 버그 검사): TSAN=1 bash scripts/build.sh  → bin/tree_bench_tsan
set -e
cd "$(dirname "$0")/.."
mkdir -p bin
SRCS="bench/tree_bench.cpp third_party/arctic/suffix_tree.cc $(find trees -name '*.cc' | sort | tr '\n' ' ')"
if [ "$TSAN" = "1" ]; then
  g++ -O1 -g -fsanitize=thread -std=c++20 -pthread $SRCS -o bin/tree_bench_tsan && echo "built bin/tree_bench_tsan"
else
  g++ -O2 -DNDEBUG -std=c++20 -pthread $SRCS -o bin/tree_bench && echo "built bin/tree_bench"
  bin/tree_bench --list
fi

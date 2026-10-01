#!/usr/bin/env bash
# Runs obl_bench under different malloc implementations (same binary, LD_PRELOAD),
# each with and without transparent huge pages.
#
#   bench/run_allocators.sh data/synth_5m.pcap [extra obl_bench args...]
set -euo pipefail

cap=${1:?usage: run_allocators.sh CAPTURE [obl_bench args...]}
shift
root=$(cd "$(dirname "$0")/.." && pwd)
bench=${OBL_BENCH:-$root/build/obl_bench}
mimalloc=${MIMALLOC_LIB:-$root/build/lib/libmimalloc.so}
jemalloc=${JEMALLOC_LIB:-$(ls /usr/lib/x86_64-linux-gnu/libjemalloc.so.2 2>/dev/null || true)}

run() {
  local label=$1
  shift
  echo
  echo "################ $label"
  env "$@" "$bench" "$cap" "${BENCH_ARGS[@]}"
}

BENCH_ARGS=("$@")

echo "THP mode: $(cat /sys/kernel/mm/transparent_hugepage/enabled 2>/dev/null || echo unknown)"
run "glibc" LD_PRELOAD=
run "glibc + THP" LD_PRELOAD= GLIBC_TUNABLES=glibc.malloc.hugetlb=1
if [[ -n "$jemalloc" ]]; then
  run "jemalloc" LD_PRELOAD="$jemalloc"
  run "jemalloc + THP" LD_PRELOAD="$jemalloc" MALLOC_CONF=thp:always,metadata_thp:always
else
  echo "jemalloc not found (apt install libjemalloc2), skipping"
fi
if [[ -f "$mimalloc" ]]; then
  run "mimalloc" LD_PRELOAD="$mimalloc"
  run "mimalloc + THP" LD_PRELOAD="$mimalloc" MIMALLOC_ALLOW_LARGE_OS_PAGES=1
else
  echo "mimalloc not built (cmake -DOBL_WITH_MIMALLOC=ON), skipping"
fi

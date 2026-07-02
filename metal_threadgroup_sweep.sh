#!/usr/bin/env bash
set -euo pipefail

EXE=${1:-/tmp/apxdb_gpu_cpu_bench_asan}
OUTPUT=${2:-metal_threadgroup_sweep.csv}

if [[ ! -x "$EXE" ]]; then
  echo "Error: executable not found or not executable: $EXE"
  echo "Usage: $0 [path/to/apxdb_gpu_cpu_bench_asan] [output.csv]"
  exit 1
fi

THREADGROUP_SIZES=(32 64 128 256)
COUNTS=(1048576 4194304 16777216)
RUNS=${3:-1}

cat > "$OUTPUT" <<'EOF'
count,threadgroup,threadgroup1,threadgroup2,run,cpu_ms,gpu_ms,hits
EOF

for count in "${COUNTS[@]}"; do
  for tg in "${THREADGROUP_SIZES[@]}"; do
    for run in $(seq 1 "$RUNS"); do
      echo "Running count=$count tg=$tg run=$run"
      APXDB_RAW_BENCH_COUNT="$count" \
      APXDB_METAL_THREADGROUP_SIZE="$tg" \
      APXDB_METAL_THREADGROUP1="$tg" \
      APXDB_METAL_THREADGROUP2="$tg" \
      "$EXE" | awk -v count="$count" -v tg="$tg" -v run="$run" '\
      /raw count=/ {print_line=1; next} \
      print_line && /CPU time avg=/ {cpu=$4; gsub(/ms$/, "", cpu)} \
      print_line && /GPU time avg=/ {gpu=$4; gsub(/ms$/, "", gpu); hits=$6; printf "%s,%s,%s,%s,%s,%s,%s,%s\n", count, tg, tg, tg, run, cpu, gpu, hits} \
      ' >> "$OUTPUT"
    done
  done

done

echo "Sweep complete: $OUTPUT"

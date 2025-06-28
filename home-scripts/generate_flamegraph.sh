#!/usr/bin/env bash
set -euo pipefail

# Usage: ./generate_flamegraph.sh <TABLE_NAME> <DURATION_IN_SECONDS>
# Default: TABLE_NAME=demo_2000000, DURATION_IN_SECONDS=30
TABLE=${1:-demo_2000000}
DURATION=${2:-30}

# Ensure FlameGraph tools are available
if [ ! -d "$HOME/FlameGraph" ]; then
  echo "Cloning FlameGraph..."
  git clone https://github.com/brendangregg/FlameGraph.git "$HOME/FlameGraph"
fi

# Flush OS cache
echo "Flushing OS cache..."
sudo sh -c 'echo 3 > /proc/sys/vm/drop_caches'

# Start background SQL workload
echo "Starting background workload on table $TABLE (5 parallel clients)..."
PIDS=()
for i in $(seq 1 5); do
  (
    while true; do
      gsql -d postgres -p 5432 -U omm -c "SELECT * FROM $TABLE ORDER BY val LIMIT 100;" >/dev/null
    done
  ) &
  PIDS+=("$!")
done

# Find gaussdb process ID
GAUSS_PID=$(pgrep -u "$(whoami)" -f gaussdb | head -n1)
echo "Sampling gaussdb PID=$GAUSS_PID for $DURATION seconds (freq=99Hz)..."

# Perf sampling
perf record -F 99 -p "$GAUSS_PID" -g -- sleep "$DURATION"

# Stop background workload
echo "Stopping workload processes: ${PIDS[*]}"
kill "${PIDS[@]}" 2>/dev/null || true

# Generate flamegraph
echo "Generating flamegraph..."
perf script > out.perf
"$HOME/FlameGraph"/stackcollapse-perf.pl out.perf > out.folded
"$HOME/FlameGraph"/flamegraph.pl out.folded > flamegraph_${TABLE}.svg

echo "🔥 Flamegraph generated at $(pwd)/flamegraph_${TABLE}.svg"

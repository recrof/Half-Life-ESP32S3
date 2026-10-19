#!/bin/bash
# Largest static variables of the host binary: tools/statics.sh [binary] [count]
BIN=${1:-$(dirname "$0")/../host/hlhost}; N=${2:-30}
T=$(mktemp)
nm -n -m "$BIN" | grep -E "\(__DATA,__(bss|common|data)\)" | awk '{print $1, $NF}' > "$T"
python3 -I - "$T" "$N" <<'PY'
import sys
rows=[(int(a,16),n) for a,n in (l.split() for l in open(sys.argv[1]))]
out=sorted(((rows[i+1][0]-rows[i][0],rows[i][1]) for i in range(len(rows)-1)),reverse=True)
for s,n in out[:int(sys.argv[2])]: print(f"{s/1024:9.1f} KB {n}")
PY
rm -f "$T"

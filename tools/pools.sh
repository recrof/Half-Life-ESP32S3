#!/bin/bash
# Largest memory pools of the last memlist in a log: tools/pools.sh <log> [count]
LOG=$1; N=${2:-15}
awk '/memlist|Memory pools/{delete a; n=0} /^\[[0-9:]+\] +[0-9.]+ (Mb|Kb|bytes) \(/{a[n++]=$0} END{for(i=0;i<n;i++) print a[i]}' "$LOG" |
	sed -E 's/^\[[0-9:]+\] +//' |
	awk '{v=$1; u=$2; if(u=="Mb")v*=1024; else if(u=="bytes")v/=1024; s=$0; sub(/.*\t/,"",s); printf "%9.1f KB  %s\n", v, s}' |
	sort -rn | head -$N

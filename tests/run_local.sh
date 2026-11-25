#!/bin/sh
# Lanza N nodos en esta máquina (uno por proceso) y verifica que el nodo 0 imprima PASS.
N=${1:-3}; CONF=${2:-conf/dsm_local.conf}; D=$(mktemp -d)
i=0; PIDS=""
while [ $i -lt $N ]; do
  ./apps/hello_dsm "$CONF" "$N" > "$D/n$i.log" 2>&1 &
  PIDS="$PIDS $!"; i=$((i+1)); sleep 0.5   # uniones secuenciales (ver README: limitación conocida)
done
RC=0; for p in $PIDS; do wait $p || RC=1; done
cat "$D"/n*.log
grep -q PASS "$D/n0.log" && [ $RC -eq 0 ] && echo "== TEST OK ==" || { echo "== TEST FALLO =="; exit 1; }

#!/bin/sh
# Prueba de locks: N nodos incrementan un contador compartido ITER veces cada uno con
# dsm_lock/dsm_unlock; el nodo 0 verifica que el total sea N x ITER.
N=${1:-3}; ITER=${2:-300}; CONF=${3:-conf/dsm_local.conf}; D=$(mktemp -d)
i=0; PIDS=""
while [ $i -lt $N ]; do
  ./apps/counter "$CONF" "$N" "$ITER" > "$D/n$i.log" 2>&1 &
  PIDS="$PIDS $!"; i=$((i+1)); sleep 0.5
done
RC=0; for p in $PIDS; do wait $p || RC=1; done
cat "$D"/n*.log
grep -q PASS "$D/n0.log" && [ $RC -eq 0 ] && echo "== TEST LOCK OK ==" || { echo "== TEST LOCK FALLO =="; exit 1; }

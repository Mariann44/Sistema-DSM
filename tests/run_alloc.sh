#!/bin/sh
# Prueba de la asignación distribuida: lanza N nodos con apps/alloc_test y exige PASS en todos.
N=${1:-3}; CONF=${2:-conf/dsm_local.conf}; D=$(mktemp -d)
i=0; PIDS=""
while [ $i -lt $N ]; do
  ./apps/alloc_test "$CONF" "$N" > "$D/n$i.log" 2>&1 &
  PIDS="$PIDS $!"; i=$((i+1)); sleep 0.5
done
RC=0; for p in $PIDS; do wait $p || RC=1; done
cat "$D"/n*.log
[ $RC -eq 0 ] && [ "$(cat "$D"/n*.log | grep -c 'RESULTADO: PASS')" -eq "$N" ] && echo "== TEST ALLOC OK ==" || { echo "== TEST ALLOC FALLO =="; exit 1; }

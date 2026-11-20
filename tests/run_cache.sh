#!/bin/sh
# Prueba del reemplazo por CACHE_MAX_PAGES y de PAGE_WRITEBACK: 3 nodos con apps/cache_test y una
# caché de 64 páginas (conf/dsm_cache.conf). Exige PASS en los 3 y que haya habido expulsiones.
CONF=${1:-conf/dsm_cache.conf}; D=$(mktemp -d)
i=0; PIDS=""
while [ $i -lt 3 ]; do
  ./apps/cache_test "$CONF" 3 > "$D/n$i.log" 2>&1 &
  PIDS="$PIDS $!"; i=$((i+1)); sleep 0.5
done
RC=0; for p in $PIDS; do wait $p || RC=1; done
cat "$D"/n*.log
if [ $RC -eq 0 ] && [ "$(cat "$D"/n*.log | grep -c 'RESULTADO: PASS')" -eq 3 ] \
   && grep -q 'caché: [1-9]' "$D/n1.log" && grep -q 'M devueltas por reemplazo' "$D/n1.log"; then
  echo "== TEST CACHE OK =="
else
  echo "== TEST CACHE FALLO =="; exit 1
fi

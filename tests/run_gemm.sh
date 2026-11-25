#!/bin/sh
# Prueba de gemm sobre el DSM: corre gemm_serial (PolyBench original, MEDIUM_DATASET) y gemm_dsm
# con las mismas dimensiones en N nodos locales, verifica todos los elementos de C en el nodo 0 y
# compara los volcados de C de ambas versiones con diff.
# Uso: tests/run_gemm.sh [nodos] [hilos] [conf]
N=${1:-3}; T=${2:-2}; CONF=${3:-conf/dsm_local.conf}; D=$(mktemp -d)
./apps/gemm/gemm_serial 2> "$D/serial.dump" > /dev/null || { echo "== TEST GEMM FALLO (serial) =="; exit 1; }
i=0; PIDS=""
while [ $i -lt $N ]; do
  ./apps/gemm/gemm_dsm "$CONF" "$N" -t "$T" -n 200 220 240 -s 0 -d "$D/dsm.dump" > "$D/n$i.log" 2>&1 &
  PIDS="$PIDS $!"; i=$((i+1)); sleep 0.5
done
RC=0; for p in $PIDS; do wait $p || RC=1; done
cat "$D"/n*.log
if [ $RC -eq 0 ] && grep -q 'RESULTADO: PASS' "$D/n0.log" && diff -q "$D/serial.dump" "$D/dsm.dump" > /dev/null; then
  echo "volcado de C identico al de PolyBench serial ($(wc -c < "$D/dsm.dump") bytes)"
  echo "== TEST GEMM OK =="
else
  diff "$D/serial.dump" "$D/dsm.dump" | head -5
  echo "== TEST GEMM FALLO =="; exit 1
fi

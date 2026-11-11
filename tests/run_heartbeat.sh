#!/bin/sh
# Prueba del heartbeat con 3 nodos en esta máquina:
#   1) se pausa el nodo 2 por 5 s  -> los demás lo marcan SUSPECT y luego vuelve a ALIVE
#   2) se pausa el nodo 2 por 13 s -> los demás lo marcan SUSPECT y luego DOWN
# Usa SUSPECT_AFTER_MS=3000 y HEARTBEAT_TIMEOUT_MS=10000 de conf/dsm_local.conf.
CONF=${1:-conf/dsm_local.conf}; D=$(mktemp -d); DUR=32
./apps/idle_dsm "$CONF" $DUR > "$D/n0.log" 2>&1 & P0=$!; sleep 0.5
./apps/idle_dsm "$CONF" $DUR > "$D/n1.log" 2>&1 & P1=$!; sleep 0.5
./apps/idle_dsm "$CONF" $DUR > "$D/n2.log" 2>&1 & P2=$!
sleep 3
echo "-- pausa corta del nodo 2 (5 s)";  kill -STOP $P2; sleep 5; kill -CONT $P2
sleep 3
echo "-- pausa larga del nodo 2 (13 s)"; kill -STOP $P2; sleep 13; kill -CONT $P2
wait $P0 $P1 $P2
for i in 0 1 2; do echo "== nodo $i"; grep -E "SUSPECT|DOWN|ALIVE|unido|fundador" "$D/n$i.log"; done
RC=0
for i in 0 1; do
  grep -q "nodo 2 sin latidos .*ALIVE -> SUSPECT" "$D/n$i.log" || { echo "nodo $i no marcó SUSPECT"; RC=1; }
  grep -q "nodo 2 volvió a latir: SUSPECT -> ALIVE" "$D/n$i.log" || { echo "nodo $i no marcó la recuperación"; RC=1; }
  grep -q "nodo 2 sin latidos .*-> DOWN" "$D/n$i.log" || { echo "nodo $i no marcó DOWN"; RC=1; }
done
grep -q "nodo [01] sin latidos" "$D/n0.log" "$D/n1.log" && { echo "falso positivo entre los nodos 0 y 1"; RC=1; }
[ $RC -eq 0 ] && echo "== TEST HEARTBEAT OK ==" || { echo "== TEST HEARTBEAT FALLO (logs en $D) =="; exit 1; }

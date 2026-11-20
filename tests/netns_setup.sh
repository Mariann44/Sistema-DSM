#!/bin/sh
# Crea dos "máquinas" en un solo Linux: los espacios de nombres de red hostA (10.10.0.1) y hostB
# (10.10.0.2), unidos por un par veth. Cada nodo corre con la red de una de ellas, así que el
# tráfico entre nodos de distinta máquina pasa por una interfaz real y no por 127.0.0.1.
# Uso: sudo tests/netns_setup.sh [retardo]     (p. ej. "2ms" agrega latencia en cada sentido)
#      sudo tests/netns_setup.sh borrar
set -e
if [ "$1" = "borrar" ]; then ip netns del hostA 2>/dev/null || true; ip netns del hostB 2>/dev/null || true; exit 0; fi
ip netns del hostA 2>/dev/null || true; ip netns del hostB 2>/dev/null || true
ip netns add hostA; ip netns add hostB
ip link add vethA type veth peer name vethB
ip link set vethA netns hostA; ip link set vethB netns hostB
ip -n hostA addr add 10.10.0.1/24 dev vethA; ip -n hostB addr add 10.10.0.2/24 dev vethB
for h in hostA hostB; do ip -n $h link set lo up; done
ip -n hostA link set vethA up; ip -n hostB link set vethB up
if [ -n "$1" ]; then
  ip netns exec hostA tc qdisc add dev vethA root netem delay "$1"
  ip netns exec hostB tc qdisc add dev vethB root netem delay "$1"
fi
echo "hostA=10.10.0.1 hostB=10.10.0.2 listos"

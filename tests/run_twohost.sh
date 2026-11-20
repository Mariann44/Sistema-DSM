#!/bin/sh
# Pruebas en dos máquinas simuladas (ver tests/netns_setup.sh, que hay que correr antes).
# Los nodos se reparten alternando entre hostA y hostB; el primero (fundador) va en hostA.
# Uso: sudo tests/run_twohost.sh            (desde la raíz del repositorio, después de make)
A="nsenter --net=/var/run/netns/hostA"; B="nsenter --net=/var/run/netns/hostB"
[ -e /var/run/netns/hostA ] || { echo "falta correr: sudo tests/netns_setup.sh"; exit 1; }
D=$(mktemp -d); FAILS=0
sed 's/^NODE=127.0.0.1$/NODE=10.10.0.1\nNODE=10.10.0.2/' conf/dsm_local.conf > "$D/two.conf"
sed 's/^NODE=127.0.0.1$/NODE=10.10.0.1\nNODE=10.10.0.2/' conf/dsm_cache.conf > "$D/two_cache.conf"

# run <nombre> <nodos> <patrón de éxito en n0> <comando...>   (el comando recibe "conf nodos" al inicio)
run() {
  name=$1; n=$2; ok=$3; shift 3; i=0; PIDS=""
  while [ $i -lt $n ]; do
    if [ $((i % 2)) -eq 0 ]; then NS=$A; else NS=$B; fi
    $NS "$@" > "$D/$name.$i.log" 2>&1 &
    PIDS="$PIDS $!"; i=$((i+1)); sleep 0.5
  done
  RC=0; for p in $PIDS; do wait $p || RC=1; done
  if [ $RC -eq 0 ] && grep -q "$ok" "$D/$name.0.log"; then echo "$name: OK"
  else echo "$name: FALLO (logs en $D)"; FAILS=$((FAILS+1)); fi
}
C=$D/two.conf
run hello   4 "verificacion final: PASS" ./apps/hello_dsm "$C" 4
run counter 4 "esperado=2000 PASS"       ./apps/counter "$C" 4 500
run alloc   4 "RESULTADO: PASS"          ./apps/alloc_test "$C" 4
run cache   3 "RESULTADO: PASS"          ./apps/cache_test "$D/two_cache.conf" 3
run gemm    4 "RESULTADO: PASS"          ./apps/gemm/gemm_dsm "$C" 4 -t 2 -n 200 220 240 -s 0
if command -v ffmpeg > /dev/null; then
  ffmpeg -loglevel error -f lavfi -i "testsrc2=size=320x240:rate=24" -t 2 -pix_fmt rgb24 -f rawvideo "$D/v.rgb"
  run sobel 4 "RESULTADO: PASS"          ./apps/sobel/sobel_dsm "$C" 4 "$D/v.rgb" 320 240 -t 2 -v 0
fi
[ $FAILS -eq 0 ] && echo "== TEST DOS MAQUINAS OK ==" || { echo "== TEST DOS MAQUINAS FALLO =="; exit 1; }

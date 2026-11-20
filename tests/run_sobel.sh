#!/bin/sh
# Prueba del filtro Sobel sobre video en el DSM: corre sobel_serial y sobel_dsm (N nodos locales)
# sobre el mismo video RGB crudo, el nodo 0 recalcula todos los cuadros, y las dos salidas deben
# ser idénticas byte a byte.
# Uso: tests/run_sobel.sh [nodos] [hilos] [video.rgb ancho alto] [conf]
# Sin video, genera uno sintético de 320x240 y 48 cuadros con ffmpeg (testsrc2).
N=${1:-3}; T=${2:-2}; V=${3:-}; W=${4:-320}; H=${5:-240}; CONF=${6:-conf/dsm_local.conf}; D=$(mktemp -d)
if [ -z "$V" ]; then
  V=$D/test.rgb
  ffmpeg -loglevel error -f lavfi -i "testsrc2=size=${W}x${H}:rate=24" -t 2 -pix_fmt rgb24 -f rawvideo "$V" \
    || { echo "== TEST SOBEL FALLO (se necesita ffmpeg para generar el video) =="; exit 1; }
fi
./apps/sobel/sobel_serial "$V" "$W" "$H" "$D/serial.gray" || { echo "== TEST SOBEL FALLO (serial) =="; exit 1; }
i=0; PIDS=""
while [ $i -lt $N ]; do
  ./apps/sobel/sobel_dsm "$CONF" "$N" "$V" "$W" "$H" -t "$T" -v 0 -o "$D/dsm.gray" > "$D/n$i.log" 2>&1 &
  PIDS="$PIDS $!"; i=$((i+1)); sleep 0.5
done
RC=0; for p in $PIDS; do wait $p || RC=1; done
cat "$D"/n*.log
if [ $RC -eq 0 ] && grep -q 'RESULTADO: PASS' "$D/n0.log" && cmp -s "$D/serial.gray" "$D/dsm.gray"; then
  echo "salida identica a sobel_serial ($(wc -c < "$D/dsm.gray") bytes)"
  echo "== TEST SOBEL OK =="
else
  echo "== TEST SOBEL FALLO =="; exit 1
fi

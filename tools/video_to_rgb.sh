#!/bin/sh
# Convierte un video (ogg, mp4, avi, ...) a RGB crudo (rgb24, cuadros seguidos) para sobel_dsm y
# sobel_serial.
# Sin video de entrada usa data/big_buck_bunny_720p_stereo.ogg, y si no existe lo descarga.
# Big Buck Bunny: (c) Blender Foundation | peach.blender.org, licencia CC BY 3.0.
#
# Uso: tools/video_to_rgb.sh [entrada] [ancho] [alto] [segundos] [fps]
#   tools/video_to_rgb.sh                         -> data/video_1280x720.rgb, 5 s (120 cuadros, ~316 MB)
#   tools/video_to_rgb.sh "" 1280 720 10          -> 10 s (240 cuadros, ~633 MB)
#
# Tamaño en memoria DSM con sobel_dsm: entrada (ancho x alto x 3 por cuadro) más salida
# (ancho x alto por cuadro). Con conf/dsm_gemm.conf cada nodo aporta 256 MB:
#   5 s en 720p  ->  ~422 MB  (3 nodos)        10 s en 720p  ->  ~844 MB  (4 nodos)
BBB=data/big_buck_bunny_720p_stereo.ogg
URL=${BBB_URL:-https://download.blender.org/peach/bigbuckbunny_movies/big_buck_bunny_720p_stereo.ogg.zip}
IN=${1:-}; W=${2:-1280}; H=${3:-720}; SECS=${4:-5}; FPS=${5:-24}
mkdir -p data
if [ -z "$IN" ]; then
  IN=$BBB
  if [ ! -f "$IN" ]; then
    echo "descargando $URL"
    curl -L -o "$IN.zip" "$URL" && unzip -o -j "$IN.zip" -d data && rm -f "$IN.zip" || {
      echo "no se pudo descargar; bajelo a mano desde https://download.blender.org/peach/bigbuckbunny_movies/"
      echo "y pongalo descomprimido en $BBB"; exit 1; }
  fi
fi
[ -f "$IN" ] || { echo "no existe $IN"; exit 1; }
OUT=data/video_${W}x${H}.rgb
# -ss 60 salta los créditos iniciales; -an descarta el audio
ffmpeg -loglevel error -y -ss 60 -t "$SECS" -i "$IN" -an -vf "scale=${W}:${H},fps=${FPS}" \
  -pix_fmt rgb24 -f rawvideo "$OUT" || exit 1
BYTES=$(stat -c %s "$OUT")
echo "$OUT: $((BYTES / (W * H * 3))) cuadros de ${W}x${H} ($((BYTES / 1048576)) MB)"
echo "ejemplo: ./apps/sobel/sobel_dsm conf/dsm_gemm.conf 3 $OUT $W $H -o data/sobel_${W}x${H}.gray"
echo "         tools/gray_to_video.sh data/sobel_${W}x${H}.gray $W $H"

#!/bin/sh
# Convierte la salida de sobel_dsm / sobel_serial (escala de grises cruda) a mp4 para verla.
# Uso: tools/gray_to_video.sh <salida.gray> <ancho> <alto> [fps]
[ $# -ge 3 ] || { echo "uso: $0 salida.gray ancho alto [fps]"; exit 2; }
ffmpeg -loglevel error -y -f rawvideo -pix_fmt gray -s "$2x$3" -r "${4:-24}" -i "$1" \
  -c:v libx264 -pix_fmt yuv420p "${1%.*}.mp4" && echo "video: ${1%.*}.mp4"

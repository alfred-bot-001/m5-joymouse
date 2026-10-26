#!/usr/bin/env bash
# StickC JoyMouse 固件：编译 / 烧录 / 看串口
#   ./build.sh compile            编译，产物放 $OUT_DIR
#   ./build.sh flash [port]       编译并烧录（默认串口 $PORT）
#   ./build.sh monitor [port]     打开串口监视器（115200）
# 产物与构建缓存不进仓库，默认放 ~/workspace-agent/agent-data/m5-joymouse/build/
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SKETCH="$HERE/stickc_joy_mouse"
FQBN="${FQBN:-esp32:esp32:m5stack_stickc:PartitionScheme=huge_app,UploadSpeed=750000}"
PORT="${2:-${PORT:-/dev/cu.usbserial-89526F4C61}}"
OUT_DIR="${OUT_DIR:-$HOME/workspace-agent/agent-data/m5-joymouse/build}"
mkdir -p "$OUT_DIR"

cmd="${1:-compile}"
case "$cmd" in
  compile)
    arduino-cli compile --fqbn "$FQBN" --build-path "$OUT_DIR/cache" --output-dir "$OUT_DIR/bin" "$SKETCH"
    ls -la "$OUT_DIR/bin"
    ;;
  flash)
    arduino-cli compile --fqbn "$FQBN" --build-path "$OUT_DIR/cache" --output-dir "$OUT_DIR/bin" "$SKETCH"
    arduino-cli upload --fqbn "$FQBN" --input-dir "$OUT_DIR/bin" -p "$PORT" "$SKETCH"
    ;;
  monitor)
    arduino-cli monitor -p "$PORT" --config baudrate=115200
    ;;
  *)
    echo "usage: $0 compile|flash|monitor [port]" >&2
    exit 2
    ;;
esac

#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

pick_python_bin() {
  if command -v python3.11 >/dev/null 2>&1; then
    echo "python3.11"
    return
  fi

  if command -v python3 >/dev/null 2>&1; then
    echo "python3"
    return
  fi

  echo ""
}

PY_BIN="$(pick_python_bin)"
if [[ -z "$PY_BIN" ]]; then
  echo "Erro: Python 3 nao encontrado no PATH."
  exit 1
fi

PY_MM="$($PY_BIN -c 'import sys; print(f"{sys.version_info.major}.{sys.version_info.minor}")')"
PY_MAJOR="${PY_MM%%.*}"
PY_MINOR="${PY_MM##*.}"
if [[ "$PY_MAJOR" -lt 3 || ( "$PY_MAJOR" -eq 3 && "$PY_MINOR" -lt 11 ) ]]; then
  echo "Erro: este app requer Python 3.11+ (detectado $PY_MM)."
  echo "Instale Python 3.11+ e rode novamente."
  exit 1
fi

if ! "$PY_BIN" -c "import tkinter" >/dev/null 2>&1; then
  echo "Erro: modulo tkinter nao encontrado para $PY_BIN."
  echo "No Ubuntu/Debian, instale com: sudo apt install python3-tk"
  exit 1
fi

if [[ ! -d ".venv" ]]; then
  "$PY_BIN" -m venv .venv
fi

source .venv/bin/activate
pip install -r requirements.txt

python esp32_mqtt_desktop.py

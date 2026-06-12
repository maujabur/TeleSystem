#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

if [[ "${EUID:-$(id -u)}" -eq 0 && -n "${SUDO_USER:-}" ]]; then
  echo "Erro: nao rode este app com sudo."
  echo "Corrija as permissoes e rode novamente como usuario normal:"
  echo "  sudo chown -R \"$SUDO_USER:$SUDO_USER\" \"$SCRIPT_DIR/.venv\""
  exit 1
fi

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
if [[ "$PY_MAJOR" -lt 3 || ( "$PY_MAJOR" -eq 3 && "$PY_MINOR" -lt 10 ) ]]; then
  echo "Erro: este app requer Python 3.10+ (detectado $PY_MM)."
  echo "Instale Python 3.10+ e rode novamente."
  exit 1
fi

if ! "$PY_BIN" -c "import tkinter" >/dev/null 2>&1; then
  echo "Erro: modulo tkinter nao encontrado para $PY_BIN."
  echo "No Ubuntu/Debian, instale com: sudo apt install python3-tk"
  exit 1
fi

VENV_DIR=".venv"
MIN_PY_MINOR=10

venv_needs_recreate() {
  if [[ ! -x "$VENV_DIR/bin/python" ]]; then
    return 0
  fi

  local venv_mm
  venv_mm="$("$VENV_DIR/bin/python" -c 'import sys; print(f"{sys.version_info.major}.{sys.version_info.minor}")')"
  local venv_major="${venv_mm%%.*}"
  local venv_minor="${venv_mm##*.}"
  [[ "$venv_major" -lt 3 || ( "$venv_major" -eq 3 && "$venv_minor" -lt "$MIN_PY_MINOR" ) ]]
}

if [[ ! -d "$VENV_DIR" ]] || venv_needs_recreate; then
  if [[ -d "$VENV_DIR" ]]; then
    if [[ ! -w "$VENV_DIR" ]]; then
      echo "Erro: ambiente Python local sem permissao de escrita: $SCRIPT_DIR/$VENV_DIR"
      echo "Corrija uma vez e rode novamente sem sudo:"
      echo "  sudo chown -R \"$(id -un):$(id -gn)\" \"$SCRIPT_DIR/$VENV_DIR\""
      exit 1
    fi
    echo "Recriando ambiente Python local em $SCRIPT_DIR/$VENV_DIR..."
    rm -rf "$VENV_DIR"
  fi
  "$PY_BIN" -m venv "$VENV_DIR"
fi

if [[ ! -w "$VENV_DIR" ]]; then
  echo "Erro: ambiente Python local sem permissao de escrita: $SCRIPT_DIR/$VENV_DIR"
  echo "Corrija uma vez e rode novamente sem sudo:"
  echo "  sudo chown -R \"$(id -un):$(id -gn)\" \"$SCRIPT_DIR/$VENV_DIR\""
  exit 1
fi

source "$VENV_DIR/bin/activate"
python -m pip install --disable-pip-version-check --quiet -r requirements.txt

python mqtt_control_center.py

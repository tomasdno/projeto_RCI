#!/usr/bin/env bash

# Test runner for OWR (dividido2).
# Usage:
#   ./run_tests.sh list
#   ./run_tests.sh test_01_join_leave
#   ./run_tests.sh 1
#   ./run_tests.sh all

set -uo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BIN="$ROOT_DIR/OWR"
LOG_ROOT="${LOG_ROOT:-/tmp/owr_tests}"
SESSION_DIR="$LOG_ROOT/session_$(date +%Y%m%d_%H%M%S)_$$"

declare -A NODE_PID=()
declare -A NODE_FIFO=()
declare -A NODE_FD=()

CASE_NAME=""
CASE_DIR=""
CASE_FAILS=0
TOTAL_FAILS=0

TESTS=(
  test_01_join_leave
  test_02_duplicate_join
  test_03_add_edge_and_neighbors
  test_04_announce_and_routing_line
  test_05_chat_end_to_end
  test_06_remove_edge_then_no_route
  test_07_coord_with_alternative_path
)

have_stdbuf() {
  command -v stdbuf >/dev/null 2>&1
}

cleanup_nodes() {
  local n fd pid

  for n in "${!NODE_FD[@]}"; do
    fd="${NODE_FD[$n]}"
    printf 'x\n' >&"$fd" 2>/dev/null || true
  done
  sleep 0.2

  for n in "${!NODE_PID[@]}"; do
    pid="${NODE_PID[$n]}"
    kill "$pid" 2>/dev/null || true
  done
  for n in "${!NODE_PID[@]}"; do
    pid="${NODE_PID[$n]}"
    wait "$pid" 2>/dev/null || true
  done

  for n in "${!NODE_FD[@]}"; do
    fd="${NODE_FD[$n]}"
    eval "exec ${fd}>&-" 2>/dev/null || true
  done
  for n in "${!NODE_FIFO[@]}"; do
    rm -f "${NODE_FIFO[$n]}" 2>/dev/null || true
  done

  NODE_PID=()
  NODE_FIFO=()
  NODE_FD=()
}

on_exit() {
  cleanup_nodes
}
trap on_exit EXIT

build_binary() {
  if [[ ! -x "$BIN" ]]; then
    echo "[INFO] Binário não encontrado. A compilar..."
  else
    echo "[INFO] A compilar (make)..."
  fi
  make -C "$ROOT_DIR" clean >/dev/null && make -C "$ROOT_DIR" >/dev/null
}

new_case() {
  cleanup_nodes
  CASE_NAME="$1"
  CASE_DIR="$SESSION_DIR/$CASE_NAME"
  mkdir -p "$CASE_DIR"
  CASE_FAILS=0
}

node_log() {
  local name="$1"
  echo "$CASE_DIR/$name.log"
}

start_node() {
  local name="$1"
  local port="$2"
  local fifo="$CASE_DIR/$name.stdin.fifo"
  local out
  local fd

  out="$(node_log "$name")"
  mkfifo "$fifo"
  exec {fd}<>"$fifo"

  NODE_FIFO["$name"]="$fifo"
  NODE_FD["$name"]="$fd"

  if have_stdbuf; then
    stdbuf -oL -eL "$BIN" 127.0.0.1 "$port" <"$fifo" >"$out" 2>&1 &
  else
    "$BIN" 127.0.0.1 "$port" <"$fifo" >"$out" 2>&1 &
  fi
  NODE_PID["$name"]=$!
  sleep 0.15
}

send_cmd() {
  local name="$1"
  shift
  local cmd="$*"
  local fd="${NODE_FD[$name]:-}"
  if [[ -z "$fd" ]]; then
    echo "[WARN] Nó '$name' não existe."
    return 1
  fi
  printf '%s\n' "$cmd" >&"$fd" 2>/dev/null || true
  sleep 0.2
  return 0
}

expect_log_contains() {
  local name="$1"
  local regex="$2"
  local what="$3"
  local file
  file="$(node_log "$name")"

  if grep -Eq "$regex" "$file"; then
    echo "  [OK] $what"
  else
    echo "  [FAIL] $what"
    echo "       regex: $regex"
    echo "       log:   $file"
    tail -n 40 "$file" | sed 's/^/         /'
    CASE_FAILS=$((CASE_FAILS + 1))
  fi
}

expect_log_not_contains() {
  local name="$1"
  local regex="$2"
  local what="$3"
  local file
  file="$(node_log "$name")"

  if grep -Eq "$regex" "$file"; then
    echo "  [FAIL] $what"
    echo "       regex (não esperado): $regex"
    echo "       log:   $file"
    tail -n 40 "$file" | sed 's/^/         /'
    CASE_FAILS=$((CASE_FAILS + 1))
  else
    echo "  [OK] $what"
  fi
}

join4_direct() {
  local base="$1"
  send_cmd n1 "dj 001 01"
  send_cmd n2 "dj 001 02"
  send_cmd n3 "dj 001 03"
  send_cmd n4 "dj 001 04"
  sleep 0.3
}

setup_line_3nodes() {
  local base="$1"
  start_node n1 $((base + 1))
  start_node n2 $((base + 2))
  start_node n3 $((base + 3))

  send_cmd n1 "dj 001 01"
  send_cmd n2 "dj 001 02"
  send_cmd n3 "dj 001 03"

  send_cmd n1 "dae 02 127.0.0.1 $((base + 2))"
  send_cmd n2 "dae 03 127.0.0.1 $((base + 3))"
  sleep 0.5
}

test_01_join_leave() {
  new_case "${FUNCNAME[0]}"
  start_node n1 7101
  send_cmd n1 "dj 001 01"
  send_cmd n1 "leave"
  send_cmd n1 "x"
  sleep 0.3

  expect_log_contains n1 "Direct join: rede=001 id=01" "join direto executado"
  expect_log_contains n1 "Saiu da rede\\." "leave executado"
}

test_02_duplicate_join() {
  new_case "${FUNCNAME[0]}"
  start_node n1 7102
  send_cmd n1 "dj 001 01"
  send_cmd n1 "dj 001 02"
  send_cmd n1 "x"
  sleep 0.3

  expect_log_contains n1 "Já está na rede 001 com id 01" "segundo join recusado"
}

test_03_add_edge_and_neighbors() {
  new_case "${FUNCNAME[0]}"
  start_node n1 7111
  start_node n2 7112

  send_cmd n1 "dj 001 01"
  send_cmd n2 "dj 001 02"
  send_cmd n1 "dae 02 127.0.0.1 7112"
  sleep 0.6
  send_cmd n1 "sg"
  send_cmd n2 "sg"
  send_cmd n1 "x"
  send_cmd n2 "x"
  sleep 0.3

  expect_log_contains n1 "id=02" "nó 01 vê o vizinho 02"
  expect_log_contains n2 "id=01" "nó 02 vê o vizinho 01"
}

test_04_announce_and_routing_line() {
  new_case "${FUNCNAME[0]}"
  setup_line_3nodes 7120

  send_cmd n3 "a"
  sleep 0.9
  send_cmd n1 "sr 03"
  send_cmd n1 "x"
  send_cmd n2 "x"
  send_cmd n3 "x"
  sleep 0.3

  expect_log_contains n1 "Destino 03: estado=EXPEDICAO[[:space:]]+dist=2[[:space:]]+succ=02" "rota de 01 para 03 pela linha"
}

test_05_chat_end_to_end() {
  new_case "${FUNCNAME[0]}"
  setup_line_3nodes 7130

  send_cmd n3 "a"
  sleep 0.9
  send_cmd n1 "m 03 ola_end2end"
  sleep 0.5
  send_cmd n1 "x"
  send_cmd n2 "x"
  send_cmd n3 "x"
  sleep 0.3

  expect_log_contains n3 "\\[CHAT\\] De 01: ola_end2end" "mensagem chegou ao destino 03"
}

test_06_remove_edge_then_no_route() {
  new_case "${FUNCNAME[0]}"
  setup_line_3nodes 7140

  send_cmd n3 "a"
  sleep 0.9
  send_cmd n2 "re 03"
  sleep 0.9
  send_cmd n1 "m 03 sem_rota"
  sleep 0.4
  send_cmd n1 "x"
  send_cmd n2 "x"
  send_cmd n3 "x"
  sleep 0.3

  expect_log_contains n1 "Erro: (sem rota para 03|sucessor 03 não está ligado)" "envio após corte sem rota"
}

test_07_coord_with_alternative_path() {
  new_case "${FUNCNAME[0]}"
  start_node n1 7151
  start_node n2 7152
  start_node n3 7153
  start_node n4 7154
  join4_direct 7150

  # Caminho primário para 03: 01 -> 02 -> 03
  send_cmd n1 "dae 02 127.0.0.1 7152"
  send_cmd n2 "dae 03 127.0.0.1 7153"
  send_cmd n3 "a"
  sleep 0.8
  send_cmd n1 "sr 03"

  # Caminho alternativo para 03: 01 -> 04 -> 03
  send_cmd n1 "dae 04 127.0.0.1 7154"
  send_cmd n4 "dae 03 127.0.0.1 7153"
  sleep 0.8

  # Remove sucessor atual para forçar coordenação e convergência
  send_cmd n1 "re 02"
  sleep 1.6
  send_cmd n1 "sr 03"

  send_cmd n1 "x"
  send_cmd n2 "x"
  send_cmd n3 "x"
  send_cmd n4 "x"
  sleep 0.3

  expect_log_contains n1 "Destino 03: estado=EXPEDICAO[[:space:]]+dist=2[[:space:]]+succ=04" "convergência para caminho alternativo via 04"
  expect_log_not_contains n1 "select: Bad file descriptor" "sem EBADF no loop principal"
}

print_list() {
  local i=1
  for t in "${TESTS[@]}"; do
    echo "$i) $t"
    i=$((i + 1))
  done
}

resolve_test_name() {
  local arg="$1"
  if [[ "$arg" =~ ^[0-9]+$ ]]; then
    local idx=$((arg - 1))
    if (( idx >= 0 && idx < ${#TESTS[@]} )); then
      echo "${TESTS[$idx]}"
      return 0
    fi
    return 1
  fi
  local t
  for t in "${TESTS[@]}"; do
    if [[ "$t" == "$arg" ]]; then
      echo "$t"
      return 0
    fi
  done
  return 1
}

run_one() {
  local t="$1"
  echo
  echo "=== $t ==="
  "$t"
  cleanup_nodes
  if (( CASE_FAILS == 0 )); then
    echo "[PASS] $t"
  else
    echo "[FAIL] $t ($CASE_FAILS falha(s))"
  fi
  TOTAL_FAILS=$((TOTAL_FAILS + CASE_FAILS))
}

usage() {
  cat <<EOF
Uso:
  $(basename "$0") list
  $(basename "$0") all
  $(basename "$0") <nome_teste>
  $(basename "$0") <numero_teste>

Exemplos:
  $(basename "$0") list
  $(basename "$0") test_04_announce_and_routing_line
  $(basename "$0") 4
EOF
}

main() {
  mkdir -p "$SESSION_DIR"
  local arg="${1:-}"
  if [[ -z "$arg" ]]; then
    usage
    exit 1
  fi

  if [[ "$arg" == "list" ]]; then
    print_list
    exit 0
  fi

  build_binary || {
    echo "[ERRO] Falha no build."
    exit 1
  }

  if [[ "$arg" == "all" ]]; then
    local t
    for t in "${TESTS[@]}"; do
      run_one "$t"
    done
    echo
    if (( TOTAL_FAILS == 0 )); then
      echo "Resumo: todos os testes passaram."
      echo "Logs: $SESSION_DIR"
      exit 0
    fi
    echo "Resumo: $TOTAL_FAILS falha(s)."
    echo "Logs: $SESSION_DIR"
    exit 1
  fi

  local test_name=""
  if ! test_name="$(resolve_test_name "$arg")"; then
    echo "[ERRO] Teste inválido: $arg"
    echo
    print_list
    exit 1
  fi

  run_one "$test_name"
  echo
  echo "Logs: $SESSION_DIR"
  if (( TOTAL_FAILS == 0 )); then
    exit 0
  fi
  exit 1
}

main "$@"

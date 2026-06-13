#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ORIGINAL_ARGS=("$@")
BUILD_DIR="${ROOT_DIR}/build"
DEV="lo"
PRESET="reorder"
OUT_DIR="${ROOT_DIR}/reliable_udp_jitter_out/tc_$(date +%Y%m%d_%H%M%S)"
DURATION=30
RATE_MBPS=5
PAYLOAD_BYTES=1200
STATS_PERIOD_MS=100
DO_BUILD=1
DO_CLEANUP=1
DO_PLOT=1
NORMAL_EDGE_SEC=5
STAGE_FILE="${ROOT_DIR}/tests/reliable_udp_jitter_stages.txt"
STAGE_NAMES=()
STAGE_DURATIONS=()
STAGE_NETEMS=()
TEST_DURATION=0

usage() {
    cat <<EOF
Usage: $0 [options]

Configure tc/netem on an interface, run test_reliable_udp_jitter_tc, then
generate controller plots with plot_reliable_udp_jitter.py.

Options:
  --preset <name>          jitter | reorder | harsh | staged (default: reorder)
  --stage-file <path>      stage text file for --preset staged
                           Format: <seconds> <name> <netem args | normal>
  --dev <ifname>           interface to shape (default: lo)
  --out-dir <path>         output directory (default: reliable_udp_jitter_out/tc_<timestamp>)
  --duration <sec>         total seconds for jitter/reorder/harsh (default: 30)
  --rate-mbps <value>      ReliableUDP send rate (default: 5)
  --payload-bytes <n>      message size including test header (default: 1200)
  --stats-period-ms <n>    stats sampling period (default: 100)
  --build | --no-build     build test target before run (default: --build)
  --plot | --no-plot       run Python plotter after capture (default: --plot)
  --cleanup | --no-cleanup remove tc qdisc on exit (default: --cleanup)
  -h, --help               show this help

Presets:
  jitter   normal 5s -> delay 20ms 5ms 25% distribution normal -> normal 5s
  reorder  normal 5s -> delay 20ms 5ms 25% distribution normal reorder 1% 25% -> normal 5s
  harsh    normal 5s -> delay 40ms 10ms 25% distribution normal reorder 5% 25% loss 1% -> normal 5s
  staged   normal 5s -> stages from --stage-file -> normal 5s

Stage file example:
  20 jitter delay 20ms 5ms 25% distribution normal
  20 jitter2 tc qdisc replace dev lo root netem delay 40ms 10ms 25% distribution normal

Root/CAP_NET_ADMIN is required for tc. Run with sudo if needed:
  sudo $0 --preset reorder --duration 60
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
    --preset)
        PRESET="$2"
        shift 2
        ;;
    --stage-file)
        STAGE_FILE="$2"
        shift 2
        ;;
    --dev)
        DEV="$2"
        shift 2
        ;;
    --out-dir)
        OUT_DIR="$2"
        shift 2
        ;;
    --duration)
        DURATION="$2"
        shift 2
        ;;
    --rate-mbps)
        RATE_MBPS="$2"
        shift 2
        ;;
    --payload-bytes)
        PAYLOAD_BYTES="$2"
        shift 2
        ;;
    --stats-period-ms)
        STATS_PERIOD_MS="$2"
        shift 2
        ;;
    --build)
        DO_BUILD=1
        shift
        ;;
    --no-build)
        DO_BUILD=0
        shift
        ;;
    --plot)
        DO_PLOT=1
        shift
        ;;
    --no-plot)
        DO_PLOT=0
        shift
        ;;
    --cleanup)
        DO_CLEANUP=1
        shift
        ;;
    --no-cleanup)
        DO_CLEANUP=0
        shift
        ;;
    -h | --help)
        usage
        exit 0
        ;;
    *)
        echo "unknown option: $1" >&2
        usage >&2
        exit 2
        ;;
esac
done

trim() {
    local value="$*"
    value="${value#"${value%%[![:space:]]*}"}"
    value="${value%"${value##*[![:space:]]}"}"
    printf '%s' "${value}"
}

csv_escape() {
    local value="${1//\"/\"\"}"
    printf '"%s"' "${value}"
}

normalize_netem_spec() {
    local spec
    spec="$(trim "$*")"
    if [[ -z "${spec}" || "${spec}" == "normal" || "${spec}" == "none" || "${spec}" == "clear" ||
          "${spec}" == "delay 0ms 0ms" ]]; then
        printf 'delay 0ms 0ms'
        return
    fi

    read -r -a words <<<"${spec}"
    if [[ ${#words[@]} -ge 3 && "${words[0]}" == "tc" && "${words[1]}" == "qdisc" && "${words[2]}" == "del" ]]; then
        printf 'delay 0ms 0ms'
        return
    fi

    for ((i = 0; i < ${#words[@]}; ++i)); do
        if [[ "${words[$i]}" == "netem" ]]; then
            printf '%s' "${words[*]:$((i + 1))}"
            return
        fi
    done

    printf '%s' "${spec}"
}

is_normal_netem() {
    local spec
    spec="$(trim "$*")"
    [[ -z "${spec}" || "${spec}" == "delay 0ms 0ms" || "${spec}" == "normal" || "${spec}" == "none" ||
       "${spec}" == "clear" ]]
}

add_stage() {
    local name="$1"
    local duration="$2"
    shift 2
    local netem
    netem="$(normalize_netem_spec "$*")"
    if ! [[ "${duration}" =~ ^[0-9]+$ ]] || [[ "${duration}" -lt 1 ]]; then
        echo "invalid stage duration for ${name}: ${duration}" >&2
        exit 2
    fi
    STAGE_NAMES+=("${name}")
    STAGE_DURATIONS+=("${duration}")
    STAGE_NETEMS+=("${netem}")
    TEST_DURATION=$((TEST_DURATION + duration))
}

add_normal_edge_stage() {
    local name="$1"
    add_stage "${name}" "${NORMAL_EDGE_SEC}" "delay 0ms 0ms"
}

read_stage_file() {
    local path="$1"
    if [[ ! -f "${path}" ]]; then
        echo "stage file not found: ${path}" >&2
        exit 2
    fi

    local line duration name netem line_no=0
    while IFS= read -r line || [[ -n "${line}" ]]; do
        line_no=$((line_no + 1))
        line="${line%%#*}"
        line="$(trim "${line}")"
        if [[ -z "${line}" ]]; then
            continue
        fi

        read -r duration name netem <<<"${line}"
        if [[ -z "${duration}" || -z "${name}" || -z "${netem}" ]]; then
            echo "invalid stage file line ${line_no}: ${line}" >&2
            echo "expected: <seconds> <name> <netem args | normal>" >&2
            exit 2
        fi
        add_stage "${name}" "${duration}" "${netem}"
    done <"${path}"
}

case "${PRESET}" in
jitter)
    if [[ ${DURATION} -le $((2 * NORMAL_EDGE_SEC)) ]]; then
        echo "--duration must be greater than $((2 * NORMAL_EDGE_SEC))s for ${PRESET}" >&2
        exit 2
    fi
    add_normal_edge_stage "normal"
    add_stage "jitter" "$((DURATION - 2 * NORMAL_EDGE_SEC))" "delay 20ms 5ms 25% distribution normal"
    add_normal_edge_stage "normal"
    ;;
reorder)
    if [[ ${DURATION} -le $((2 * NORMAL_EDGE_SEC)) ]]; then
        echo "--duration must be greater than $((2 * NORMAL_EDGE_SEC))s for ${PRESET}" >&2
        exit 2
    fi
    add_normal_edge_stage "normal"
    add_stage "reorder" "$((DURATION - 2 * NORMAL_EDGE_SEC))" "delay 20ms 5ms 25% distribution normal reorder 1% 25%"
    add_normal_edge_stage "normal"
    ;;
harsh)
    if [[ ${DURATION} -le $((2 * NORMAL_EDGE_SEC)) ]]; then
        echo "--duration must be greater than $((2 * NORMAL_EDGE_SEC))s for ${PRESET}" >&2
        exit 2
    fi
    add_normal_edge_stage "normal"
    add_stage "harsh" "$((DURATION - 2 * NORMAL_EDGE_SEC))" "delay 40ms 10ms 25% distribution normal reorder 5% 25% loss 1%"
    add_normal_edge_stage "normal"
    ;;
staged)
    add_normal_edge_stage "normal"
    read_stage_file "${STAGE_FILE}"
    add_normal_edge_stage "normal"
    ;;
*)
    echo "unknown preset: ${PRESET}" >&2
    usage >&2
    exit 2
    ;;
esac

if [[ ${EUID} -ne 0 ]]; then
    if command -v sudo >/dev/null 2>&1; then
        exec sudo -E "$0" "${ORIGINAL_ARGS[@]}"
    fi
    echo "tc requires root/CAP_NET_ADMIN; rerun as root." >&2
    exit 1
fi

clear_tc() {
    tc qdisc del dev "${DEV}" root >/dev/null 2>&1 || true
}

apply_stage_tc() {
    local netem="$1"
    if is_normal_netem "${netem}"; then
        clear_tc
        return
    fi
    read -r -a NETEM_WORDS <<<"${netem}"
    tc qdisc replace dev "${DEV}" root netem "${NETEM_WORDS[@]}"
}

cleanup_tc() {
    if [[ ${DO_CLEANUP} -eq 1 ]]; then
        clear_tc
    fi
}

trap cleanup_tc EXIT INT TERM

cd "${ROOT_DIR}"

if [[ ${DO_BUILD} -eq 1 ]]; then
    cmake --build "${BUILD_DIR}" --target test_reliable_udp_jitter_tc
fi

mkdir -p "${OUT_DIR}"

echo "preset,${PRESET}" >"${OUT_DIR}/tc_qdisc.txt"
if [[ "${PRESET}" == "staged" ]]; then
    echo "stage_file,${STAGE_FILE}" >>"${OUT_DIR}/tc_qdisc.txt"
fi
echo "stage,start_s,end_s,netem_args" >"${OUT_DIR}/tc_stages.csv"

echo "Starting ${PRESET} ReliableUDP jitter capture into ${OUT_DIR}"
echo "Total duration: ${TEST_DURATION}s; normal guard: ${NORMAL_EDGE_SEC}s at start/end"
clear_tc
"${BUILD_DIR}/tests/test_reliable_udp_jitter_tc" \
    --duration "${TEST_DURATION}" \
    --rate-mbps "${RATE_MBPS}" \
    --payload-bytes "${PAYLOAD_BYTES}" \
    --stats-period-ms "${STATS_PERIOD_MS}" \
    --out-dir "${OUT_DIR}" &
TEST_PID=$!

START_S=0
for ((i = 0; i < ${#STAGE_NAMES[@]}; ++i)); do
    STAGE_LEN=${STAGE_DURATIONS[$i]}
    END_S=$((START_S + STAGE_LEN))
    NAME=${STAGE_NAMES[$i]}
    NETEM=${STAGE_NETEMS[$i]}

    if is_normal_netem "${NETEM}"; then
        echo "Stage ${NAME}: zero delay/jitter (${START_S}-${END_S}s)"
    else
        echo "Stage ${NAME}: ${NETEM} (${START_S}-${END_S}s)"
    fi
    apply_stage_tc "${NETEM}"
    tc qdisc show dev "${DEV}" | tee -a "${OUT_DIR}/tc_qdisc.txt"
    printf '%s,%s,%s,' "${NAME}" "${START_S}" "${END_S}" >>"${OUT_DIR}/tc_stages.csv"
    csv_escape "${NETEM}" >>"${OUT_DIR}/tc_stages.csv"
    printf '\n' >>"${OUT_DIR}/tc_stages.csv"

    for ((sec = 0; sec < STAGE_LEN; ++sec)); do
        if ! kill -0 "${TEST_PID}" >/dev/null 2>&1; then
            break 2
        fi
        sleep 1
    done
    START_S=${END_S}
done

clear_tc
wait "${TEST_PID}"

if [[ ${DO_PLOT} -eq 1 ]]; then
    python3 "${ROOT_DIR}/tests/plot_reliable_udp_jitter.py" "${OUT_DIR}"
fi

if [[ -n "${SUDO_UID:-}" && -n "${SUDO_GID:-}" ]]; then
    chown -R "${SUDO_UID}:${SUDO_GID}" "${OUT_DIR}" >/dev/null 2>&1 || true
fi

echo
echo "Done. Key outputs:"
echo "  ${OUT_DIR}/network_vs_controller.png"
echo "  ${OUT_DIR}/latency_ideal_vs_actual.png"
echo "  ${OUT_DIR}/parameter_influence.png"
echo "  ${OUT_DIR}/controller_assessment.txt"

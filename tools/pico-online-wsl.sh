#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BRIDGE_BIN="$REPO_ROOT/build/release/bin/pico-udp-midi-bridge"
PICODECODER_DIR="$REPO_ROOT/resources/picodecoder/linux/x86_64"

host_ip=""
udp_port="5005"
debug="0"
build_if_missing="1"
bridge_mode="stable"
debug_scope="all"
led_port="5006"

usage() {
    cat <<'EOF'
Usage: pico-online-wsl.sh [--host IP] [--port PORT] [--debug] [--debug-scope all|gates|controls] [--mode stable|parity] [--led-port PORT] [--no-led] [--no-build]
EOF
}

detect_host_ip() {
    local route_ip
    route_ip="$(ip route show default 2>/dev/null | awk '{print $3; exit}')"
    if [[ -n "$route_ip" ]]; then
        printf '%s\n' "$route_ip"
        return 0
    fi

    local resolv_ip
    resolv_ip="$(awk '/^nameserver / {print $2; exit}' /etc/resolv.conf 2>/dev/null || true)"
    if [[ -n "$resolv_ip" ]]; then
        printf '%s\n' "$resolv_ip"
        return 0
    fi

    return 1
}

build_bridge() {
    cmake -B "$REPO_ROOT/build" -DCMAKE_BUILD_TYPE=Release -DUSE_DYNAMIC=ON
    cmake --build "$REPO_ROOT/build" --target pico-udp-midi-bridge -j
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --host)
            host_ip="${2:-}"
            shift 2
            ;;
        --port)
            udp_port="${2:-}"
            shift 2
            ;;
        --debug)
            debug="1"
            shift
            ;;
        --debug-scope)
            debug_scope="${2:-}"
            debug="1"
            shift 2
            ;;
        --mode)
            bridge_mode="${2:-}"
            shift 2
            ;;
        --led-port)
            led_port="${2:-}"
            shift 2
            ;;
        --no-led)
            led_port="0"
            shift
            ;;
        --no-build)
            build_if_missing="0"
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "Unknown argument: $1" >&2
            usage >&2
            exit 1
            ;;
    esac
done

if [[ -z "$host_ip" ]]; then
    host_ip="$(detect_host_ip)" || {
        echo "Could not determine the Windows-side WSL host IP. Pass --host manually." >&2
        exit 1
    }
fi

if [[ ! -x "$BRIDGE_BIN" ]]; then
    if [[ "$build_if_missing" != "1" ]]; then
        echo "Bridge binary missing at $BRIDGE_BIN" >&2
        exit 1
    fi
    echo "Building pico-udp-midi-bridge..."
    build_bridge
fi

export LD_LIBRARY_PATH="$PICODECODER_DIR${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

echo "Starting Pico bridge to $host_ip:$udp_port using mode '$bridge_mode' debug-scope '$debug_scope' led-port '$led_port'"
exec sudo env LD_LIBRARY_PATH="$LD_LIBRARY_PATH" "$BRIDGE_BIN" "$host_ip" "$udp_port" "$debug" "$bridge_mode" "$debug_scope" "$led_port"

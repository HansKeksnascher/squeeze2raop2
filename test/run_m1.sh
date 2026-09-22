#!/usr/bin/env bash
# run M1 loop test: fake LMS + squeeze2raop2 (file sink), dump logs, cleanup.
set -u
LOGDIR=${LOGDIR:-/tmp/opencode}
rm -f "$LOGDIR"/m1_*.log "$LOGDIR"/kitchen_*.wav
mkdir -p "$LOGDIR"

python3 test/fake_lms.py --tcp-port 3483 --http-port 9000 \
    --stream-seconds "${STREAM_SECONDS:-4}" --volume-pct "${VOLUME_PCT:-68}" \
    --stop-after-sec "${STOP_AFTER:-0}" > "$LOGDIR/m1_lms.log" 2>&1 &
LMS_PID=$!
sleep 0.7

cat > "$LOGDIR/m1.conf" <<EOF
[global]
lms = 127.0.0.1
discovery = off
log = ${BRIDGE_LOG:-info}

[player "Kitchen"]
sink = $LOGDIR/kitchen_test.wav
pace = ${PACE:-fast}
EOF

./build/squeeze2raop2 --config "$LOGDIR/m1.conf" > "$LOGDIR/m1_bridge.log" 2>&1 &
BRIDGE_PID=$!

read -t "${RUN_SECONDS:-6}" -n 1 key || true
wait_for() {
    local f=$1 want=$2 tries=${3:-20}
    for _ in $(seq "$tries"); do
        grep -q "$want" "$1" 2>/dev/null && return 0
        sleep 0.5
    done
    return 1
}

wait_for "$LOGDIR/m1_lms.log" "STAT STMu" || true
kill -9 "$LMS_PID" "$BRIDGE_PID" 2>/dev/null
wait 2>/dev/null
echo "===== LMS ====="; cat "$LOGDIR/m1_lms.log"
echo "===== BRIDGE ====="; cat "$LOGDIR/m1_bridge.log"
echo "===== WAV ====="
ls -la "$LOGDIR/kitchen_test-Kitchen.wav" 2>/dev/null || echo "no wav file"
exit 0
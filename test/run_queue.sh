#!/usr/bin/env bash
# Queue/end-of-track test: fake LMS serves N short tracks to EOF and advances
# the playlist on the player's STMd ("decoder ready"), the way LMS does.
# Asserts the bridge reports STMd and picks up the next track.
set -u
LOGDIR=${LOGDIR:-/tmp/opencode}
TRACKS=${TRACKS:-3}
TRACK_SECONDS=${TRACK_SECONDS:-1}
rm -f "$LOGDIR"/queue_*.log "$LOGDIR"/queue_*.wav
mkdir -p "$LOGDIR"

python3 test/fake_lms.py --stream-seconds "$TRACK_SECONDS" --queue-tracks "$TRACKS" --no-volume \
    > "$LOGDIR/queue_lms.log" 2>&1 &
LMS_PID=$!
sleep 0.7

cat > "$LOGDIR/queue.conf" <<EOF
[global]
lms = 127.0.0.1
discovery = off
log = info

[player "QueueTest"]
sink = $LOGDIR/queue_audio.wav
pace = fast
EOF

./build/squeeze2raop2 --config "$LOGDIR/queue.conf" \
    > "$LOGDIR/queue_bridge.log" 2>&1 &
BRIDGE_PID=$!

n=0
while [ $n -lt 40 ]; do
    started=$(grep -c "strm-s sent" "$LOGDIR/queue_lms.log" 2>/dev/null)
    [ "$started" -ge "$TRACKS" ] && break
    sleep 0.5
    n=$((n+1))
done

kill -9 "$LMS_PID" "$BRIDGE_PID" 2>/dev/null
wait 2>/dev/null

echo "===== LMS ====="; cat "$LOGDIR/queue_lms.log"
echo "===== BRIDGE (tail) ====="; tail -20 "$LOGDIR/queue_bridge.log"
echo "===== RESULT ====="
started=$(grep -c "strm-s sent" "$LOGDIR/queue_lms.log")
stmd=$(grep -c "STMd received" "$LOGDIR/queue_lms.log")
want_stmd=$((TRACKS - 1))
echo "tracks started=$started (want $TRACKS), STMd received=$stmd (want $want_stmd)"
if [ "$started" -ge "$TRACKS" ] && [ "$stmd" -ge "$want_stmd" ]; then
    echo "QUEUE-ADVANCE: PASS"
else
    echo "QUEUE-ADVANCE: FAIL"
fi
exit 0
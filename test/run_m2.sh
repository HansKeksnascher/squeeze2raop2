#!/usr/bin/env bash
# M2 test: bridge with two static flow devices against fake LMS.
set -u
LOGDIR=/tmp/opencode
rm -f "$LOGDIR"/m2_*.log
mkdir -p "$LOGDIR"

python3 test/fake_lms.py --tcp-port 3483 --http-port 9000 > "$LOGDIR/m2_lms.log" 2>&1 &
LMS_PID=$!
sleep 0.7

./build/sqraop2 --lms 127.0.0.1 --discovery off --device Kitchen --device "Living Room" \
    --sink "$LOGDIR/m2_audio.wav" --pace fast --log info > "$LOGDIR/m2_bridge.log" 2>&1 &
BRIDGE_PID=$!

# wait for two HELOs from fake lms side
n=0
while [ $n -lt 20 ]; do
    players=$(grep -c "HELO device_id=12" "$LOGDIR/m2_lms.log")
    [ "$players" -ge 2 ] && break
    sleep 0.5
    n=$((n+1))
done
sleep 1
kill -9 "$LMS_PID" "$BRIDGE_PID" 2>/dev/null
wait 2>/dev/null
echo "===== LMS ====="; cat "$LOGDIR/m2_lms.log"
echo "===== BRIDGE ====="; cat "$LOGDIR/m2_bridge.log"
echo "===== STATE ====="; cat sqraop2.state 2>/dev/null | tail -5
exit 0

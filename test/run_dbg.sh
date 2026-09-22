#!/usr/bin/env bash
# debug M1: run fake LMS + squeeze2raop2 under strace, dump, cleanup.
set -u
LOGDIR=/tmp/opencode
rm -f "$LOGDIR"/dbg_*.log "$LOGDIR"/dbg.wav "$LOGDIR"/dbg_syscall
mkdir -p "$LOGDIR"

python3 test/fake_lms.py --tcp-port 3483 --http-port 9000 \
    --stream-seconds 8 --volume-pct 68 > "$LOGDIR/dbg_lms.log" 2>&1 &
LMS_PID=$!
sleep 0.7

cat > "$LOGDIR/dbg.conf" <<EOF
[global]
lms = 127.0.0.1
log = debug

[player "Kitchen"]
sink = $LOGDIR/dbg.wav
pace = fast
EOF

strace -f -e trace=connect,accept4,accept,bind,read,write,poll -o "$LOGDIR/dbg_syscall" \
    ./build/squeeze2raop2 --config "$LOGDIR/dbg.conf" > "$LOGDIR/dbg_brd.log" 2>&1 &
BRIDGE_PID=$!

sleep 5
kill -9 "$LMS_PID" "$BRIDGE_PID" 2>/dev/null
wait 2>/dev/null
echo "===== LMS ====="; cat "$LOGDIR/dbg_lms.log"
echo "===== BRIDGE ====="; cat "$LOGDIR/dbg_brd.log"
echo "===== CONNECTS ====="
grep -E "connect\(" "$LOGDIR/dbg_syscall" | tail -12
echo "===== LAST SYSCALLS ====="
tail -15 "$LOGDIR/dbg_syscall"
exit 0
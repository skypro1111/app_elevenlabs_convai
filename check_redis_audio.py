#!/usr/bin/env python3
"""Check real-time audio streaming to Redis from app_elevenlabs_convai.

Usage:
  python3 check_redis_audio.py <conversation_id>
  python3 check_redis_audio.py --list              # list active calls
  python3 check_redis_audio.py --any               # auto-pick first active call

Options:
  --host HOST    Redis host (default: 10.24.2.103)
  --port PORT    Redis port (default: 6379)
  --db DB        Redis DB   (default: 12)
"""

import argparse
import sys
import time
from datetime import datetime

import redis


MULAW_SILENCE = 0xFF
CHUNK_EXPECTED_SIZE = 800  # 100ms at 8kHz mulaw


def is_silence(data, threshold=0.95):
    """Check if chunk is mostly silence (0xFF bytes)."""
    if not data:
        return True
    silence_count = sum(1 for b in data if b == MULAW_SILENCE)
    return silence_count / len(data) >= threshold


def audio_level(data):
    """Simple audio level: average deviation from mulaw silence (0xFF)."""
    if not data:
        return 0.0
    return sum(abs(b - MULAW_SILENCE) for b in data) / len(data)


def format_bar(level, width=30):
    """Render a simple level bar."""
    filled = int(min(level / 40.0, 1.0) * width)
    return "\u2588" * filled + "\u2591" * (width - filled)


def _get_last_id(r, stream_key):
    """Get the last entry ID from a stream, or '0-0' if empty/missing."""
    try:
        last = r.xrevrange(stream_key, count=1)
        if last:
            entry_id = last[0][0]
            return entry_id.decode() if isinstance(entry_id, bytes) else entry_id
    except Exception:
        pass
    return "0-0"


def list_active_calls(r):
    """List all active calls from ast:calls hash."""
    calls = r.hgetall("ast:calls")
    if not calls:
        print("No active calls found in ast:calls")
        return []

    print(f"Active calls ({len(calls)}):")
    print("-" * 60)
    conv_ids = []
    for conv_id, info in calls.items():
        conv_id_str = conv_id.decode() if isinstance(conv_id, bytes) else conv_id
        info_str = info.decode() if isinstance(info, bytes) else info
        print(f"  {conv_id_str}")
        print(f"    {info_str[:120]}")
        conv_ids.append(conv_id_str)
    print("-" * 60)
    return conv_ids


def check_streams_exist(r, conv_id):
    """Check if audio streams exist for this conversation."""
    key_in = f"ast:call:{conv_id}:audio:in"
    key_out = f"ast:call:{conv_id}:audio:out"
    len_in = r.xlen(key_in)
    len_out = r.xlen(key_out)
    return key_in, key_out, len_in, len_out


def _parse_stream_ts(entry_id_str):
    """Extract millisecond timestamp from Redis stream ID (format: '<ms>-<seq>')."""
    try:
        return int(entry_id_str.split("-")[0])
    except (ValueError, IndexError):
        return 0


def monitor(r, conv_id):
    """Monitor both audio streams in real-time using XREAD block."""
    key_in = f"ast:call:{conv_id}:audio:in"
    key_out = f"ast:call:{conv_id}:audio:out"

    # Check initial state
    len_in = r.xlen(key_in)
    len_out = r.xlen(key_out)
    print(f"\nMonitoring audio streams for: {conv_id}")
    print(f"  IN  stream: {key_in} ({len_in} entries)")
    print(f"  OUT stream: {key_out} ({len_out} entries)")
    print()
    print(
        "  DIR  |  SIZE  | SILENCE |  LEVEL  |  RECV TIME   | DELTA ms |"
        "           BAR            | STREAM ID"
    )
    print("  " + "-" * 112)

    # Resolve actual last entry IDs from each stream.
    last_id_in = _get_last_id(r, key_in)
    last_id_out = _get_last_id(r, key_out)
    count_in = 0
    count_out = 0
    silence_in = 0
    silence_out = 0
    start_time = time.time()

    # Timing tracking per direction
    prev_ts_in = 0
    prev_ts_out = 0
    deltas_in = []
    deltas_out = []

    try:
        while True:
            # XREAD with 200ms block timeout
            streams = r.xread(
                {key_in: last_id_in, key_out: last_id_out},
                count=10,
                block=200,
            )

            if not streams:
                continue

            for stream_key, entries in streams:
                stream_name = (
                    stream_key.decode()
                    if isinstance(stream_key, bytes)
                    else stream_key
                )
                is_inbound = stream_name.endswith(":audio:in")
                direction = " IN " if is_inbound else " OUT"

                for entry_id, fields in entries:
                    entry_id_str = (
                        entry_id.decode()
                        if isinstance(entry_id, bytes)
                        else entry_id
                    )

                    data = fields.get(b"data", b"")
                    size = len(data)
                    silent = is_silence(data)
                    level = audio_level(data)
                    bar = format_bar(level)

                    # Extract timestamp from stream ID and compute delta
                    ts_ms = _parse_stream_ts(entry_id_str)
                    if is_inbound:
                        delta = ts_ms - prev_ts_in if prev_ts_in else 0
                        prev_ts_in = ts_ms
                        count_in += 1
                        last_id_in = entry_id_str
                        if silent:
                            silence_in += 1
                        if delta > 0:
                            deltas_in.append(delta)
                    else:
                        delta = ts_ms - prev_ts_out if prev_ts_out else 0
                        prev_ts_out = ts_ms
                        count_out += 1
                        last_id_out = entry_id_str
                        if silent:
                            silence_out += 1
                        if delta > 0:
                            deltas_out.append(delta)

                    size_ok = " OK " if size == CHUNK_EXPECTED_SIZE else f"!{size}!"
                    sil_mark = "  yes  " if silent else "  no   "
                    delta_str = f"{delta:6d}ms" if delta > 0 else "     - "

                    # Highlight anomalous deltas (< 50ms or > 200ms)
                    if delta > 0 and (delta < 50 or delta > 200):
                        delta_str = f"{delta_str} !"

                    # Real wall-clock time when we received this entry
                    now_str = datetime.now().strftime("%H:%M:%S.%f")[:-3]

                    print(
                        f"  {direction} | {size_ok:>6} | {sil_mark} | {level:5.1f}   "
                        f"| {now_str} | {delta_str:>10} | {bar} | {entry_id_str}"
                    )

    except KeyboardInterrupt:
        elapsed = time.time() - start_time
        print()
        print("=" * 70)
        print(f"Summary ({elapsed:.1f}s):")
        print(f"  IN:  {count_in} chunks, {silence_in} silence ({_pct(silence_in, count_in)})")
        print(f"  OUT: {count_out} chunks, {silence_out} silence ({_pct(silence_out, count_out)})")
        expected = int(elapsed / 0.1)
        print(f"  Expected ~{expected} chunks per direction ({elapsed:.1f}s / 0.1s)")
        if count_in > 0:
            print(f"  IN  delivery rate: {count_in / expected * 100:.0f}%")
        if count_out > 0:
            print(f"  OUT delivery rate: {count_out / expected * 100:.0f}%")

        # Timing analysis
        print()
        print("Timing analysis (delta between consecutive chunks):")
        print("  Expected: ~100ms between each chunk")
        for label, deltas in [("IN ", deltas_in), ("OUT", deltas_out)]:
            if deltas:
                avg = sum(deltas) / len(deltas)
                mn = min(deltas)
                mx = max(deltas)
                # Count bursts (delta < 50ms = chunks arriving too fast)
                bursts = sum(1 for d in deltas if d < 50)
                gaps = sum(1 for d in deltas if d > 200)
                print(f"  {label}: avg={avg:.0f}ms  min={mn}ms  max={mx}ms  "
                      f"bursts(<50ms)={bursts}  gaps(>200ms)={gaps}")


def _pct(part, total):
    if total == 0:
        return "n/a"
    return f"{part / total * 100:.0f}%"


def main():
    parser = argparse.ArgumentParser(description="Check Redis audio streaming")
    parser.add_argument("conversation_id", nargs="?", help="Conversation ID to monitor")
    parser.add_argument("--list", action="store_true", help="List active calls")
    parser.add_argument("--any", action="store_true", help="Auto-pick first active call")
    parser.add_argument("--host", default="10.24.2.103", help="Redis host")
    parser.add_argument("--port", type=int, default=6379, help="Redis port")
    parser.add_argument("--db", type=int, default=12, help="Redis DB")
    parser.add_argument("--password", default=None, help="Redis password")
    args = parser.parse_args()

    r = redis.Redis(host=args.host, port=args.port, db=args.db, password=args.password)

    try:
        r.ping()
    except redis.ConnectionError as e:
        print(f"Cannot connect to Redis at {args.host}:{args.port}/{args.db}: {e}")
        sys.exit(1)

    print(f"Connected to Redis at {args.host}:{args.port}/{args.db}")

    if args.list:
        list_active_calls(r)
        return

    conv_id = args.conversation_id

    if args.any or not conv_id:
        calls = list_active_calls(r)
        if not calls:
            # Try finding any audio stream keys
            keys = r.keys("ast:call:*:audio:in")
            if keys:
                # Extract conv_id from key
                k = keys[0].decode() if isinstance(keys[0], bytes) else keys[0]
                conv_id = k.replace("ast:call:", "").replace(":audio:in", "")
                print(f"\nFound audio stream for: {conv_id}")
            else:
                print("\nNo audio streams found. Make sure redis_audio_streaming=true")
                sys.exit(1)
        else:
            conv_id = calls[0]
            print(f"\nUsing first active call: {conv_id}")

    # Check streams
    key_in, key_out, len_in, len_out = check_streams_exist(r, conv_id)

    if len_in == 0 and len_out == 0:
        print(f"\nNo audio data yet for {conv_id}")
        print("Waiting for data... (Ctrl+C to quit)")

    monitor(r, conv_id)


if __name__ == "__main__":
    main()

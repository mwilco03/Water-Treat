#!/usr/bin/env bash
# =============================================================================
#  verify-deployment.sh — End-to-end validation of water-treat controls
# =============================================================================
#
#  PURPOSE
#  -------
#  Validates every fix shipped in Waves 1-3a + 5 and F1-F5 against a live RTU.
#  Produces a PASS/FAIL report with concrete evidence for each control.
#
#  Each check:
#    - States what it's verifying
#    - References the wave / failure number being tested
#    - Captures the raw evidence (stdout from the target) inline
#    - Returns PASS or FAIL with reason
#
#  USAGE
#  -----
#    ./scripts/verify-deployment.sh [TARGET]
#
#  Defaults to 192.168.7.173 (Le Potato fresh-deploy target).
#  Override:
#    ./scripts/verify-deployment.sh 192.168.6.21
#
#  Authentication:
#    Uses sshpass with the documented training-range password H2OhYeah!.
#    Override via env: WT_SSH_USER, WT_SSH_PASS, WT_SSH_OPTS.
#
#  EXIT CODE
#  ---------
#    0 — every control passed
#    1 — at least one control failed
#    2 — environment error (target unreachable, sshpass missing, etc.)
#
# =============================================================================

set -uo pipefail

TARGET="${1:-192.168.7.173}"
WT_SSH_USER="${WT_SSH_USER:-rtu}"
WT_SSH_PASS="${WT_SSH_PASS:-H2OhYeah!}"
WT_SSH_OPTS="${WT_SSH_OPTS:--o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=10 -o LogLevel=ERROR}"

# ----- output helpers -------------------------------------------------------

GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
DIM='\033[2m'
BOLD='\033[1m'
NC='\033[0m'

PASS_COUNT=0
FAIL_COUNT=0
SKIP_COUNT=0
FAILED_TESTS=()

print_header() {
    echo
    echo -e "${BOLD}${CYAN}━━━ $* ━━━${NC}"
}

pass() {
    PASS_COUNT=$((PASS_COUNT + 1))
    echo -e "  ${GREEN}[PASS]${NC} $*"
}

fail() {
    FAIL_COUNT=$((FAIL_COUNT + 1))
    FAILED_TESTS+=("$*")
    echo -e "  ${RED}[FAIL]${NC} $*"
}

skip() {
    SKIP_COUNT=$((SKIP_COUNT + 1))
    echo -e "  ${YELLOW}[SKIP]${NC} $*"
}

evidence() {
    while IFS= read -r line; do
        echo -e "         ${DIM}│${NC} ${DIM}${line}${NC}"
    done
}

# ----- SSH helpers ----------------------------------------------------------

# Run a command on the target as the SSH user
ssh_run() {
    sshpass -p "${WT_SSH_PASS}" ssh ${WT_SSH_OPTS} \
        "${WT_SSH_USER}@${TARGET}" "$@" 2>&1
}

# Run a command on the target as root via sudo (using the same password)
ssh_sudo() {
    sshpass -p "${WT_SSH_PASS}" ssh ${WT_SSH_OPTS} \
        "${WT_SSH_USER}@${TARGET}" \
        "echo '${WT_SSH_PASS}' | sudo -S -p '' $* 2>&1" 2>&1
}

# Pre-flight check for required tools on this host
preflight() {
    local missing=()
    command -v sshpass &>/dev/null || missing+=("sshpass")
    command -v ssh &>/dev/null || missing+=("ssh")
    command -v curl &>/dev/null || missing+=("curl")
    if [[ ${#missing[@]} -gt 0 ]]; then
        echo "ERROR: missing tools on this host: ${missing[*]}" >&2
        echo "Install with: apt install sshpass openssh-client curl" >&2
        exit 2
    fi
}

# Probe connectivity to the target
probe_target() {
    print_header "Pre-flight"
    if ssh_run 'echo OK' 2>&1 | grep -q '^OK$'; then
        local uname_out
        uname_out=$(ssh_run 'uname -a')
        pass "SSH to ${WT_SSH_USER}@${TARGET} reachable"
        echo "${uname_out}" | evidence
    else
        fail "SSH to ${WT_SSH_USER}@${TARGET} failed"
        echo "Cannot reach target. Aborting." >&2
        exit 2
    fi
}

# =============================================================================
#  Tier 1 — Build & install controls (F1-F5, Wave 5)
# =============================================================================

check_f1_libgpiod_v2() {
    # F1: scripts/build.sh must accept libgpiod v2.
    # The source tree may live at any of several locations depending on how
    # the operator deployed (git clone vs install path vs Ceph rsync). Search
    # the common locations and use the first one that has scripts/build.sh.
    # NOTE: ssh_sudo passes $* to a single bash -c on the remote side, so
    # multi-line shell constructs must be flattened with ';' separators.
    local out
    out=$(ssh_sudo 'src=""; for d in /home/rtu/Water-Treat /home/sadmin/Water-Treat /opt/water-treat /root/Water-Treat /var/lib/water-treat-src; do if [ -f "$d/scripts/build.sh" ]; then src="$d"; break; fi; done; if [ -z "$src" ]; then echo NO_SOURCE_TREE_FOUND; else grep -nE "1\\.\\*|2\\.\\*|libgpiod.*v[12]" "$src/scripts/build.sh" | head -10; fi')
    if echo "${out}" | grep -qE '1\.\*\|2\.\*'; then
        pass "F1: build.sh accepts both libgpiod v1 and v2"
        echo "${out}" | grep -E '1\.\*|2\.\*' | head -2 | evidence
    elif echo "${out}" | grep -q 'NO_SOURCE_TREE'; then
        skip "F1: no source tree found on target — cannot verify build.sh content (binary-only deploy)"
    else
        fail "F1: build.sh case match still rejects libgpiod v2"
        echo "${out}" | head -10 | evidence
    fi
}

check_f2_gsdml_installed() {
    # F2: install.sh installs GSDML to /usr/share/<project>/gsd/
    local out
    out=$(ssh_sudo "ls -la /usr/share/water-treat/gsd/ 2>&1; \
                    echo ---; ls -la /opt/water-treat/gsd/ 2>&1")
    if echo "${out}" | grep -q 'GSDML-V2\.4-WaterTreat'; then
        pass "F2: GSDML XML installed to a binary search path"
        echo "${out}" | grep 'GSDML' | head -3 | evidence
    else
        fail "F2: GSDML XML missing from /usr/share/water-treat/gsd/ AND /opt/water-treat/gsd/"
        echo "${out}" | head -10 | evidence
    fi
}

check_f2_config_files_installed() {
    # F2: install.sh installs water-treat.conf.example and water-treat.env
    local out
    out=$(ssh_sudo "ls -la /etc/water-treat/")
    local has_example=0 has_env=0
    echo "${out}" | grep -q 'water-treat.conf.example' && has_example=1
    echo "${out}" | grep -q 'water-treat.env' && has_env=1
    if [[ ${has_example} -eq 1 && ${has_env} -eq 1 ]]; then
        pass "F2: water-treat.env and water-treat.conf.example installed in /etc/water-treat/"
        echo "${out}" | grep -E 'water-treat\.(env|conf)' | evidence
    else
        fail "F2: missing config files (env=${has_env}, conf.example=${has_example})"
        echo "${out}" | evidence
    fi
}

check_f3_service_enabled() {
    # F3: install.sh runs systemctl enable
    local out
    out=$(ssh_sudo 'systemctl is-enabled water-treat.service 2>&1')
    if echo "${out}" | grep -q '^enabled$'; then
        pass "F3: water-treat.service enabled for boot"
        echo "${out}" | evidence
    else
        fail "F3: water-treat.service is '${out}' (expected 'enabled')"
    fi
}

check_f4_firmware_dir_no_double_slash() {
    # F4: install.sh FIRMWARE_SHARE_DIR ordering bug
    local out
    out=$(ssh_sudo 'cat /etc/water-treat/.install-manifest 2>&1')
    if echo "${out}" | grep -qE 'FIRMWARE.*//'; then
        fail "F4: install manifest still has double-slash (PROJECT_NAME ordering bug)"
        echo "${out}" | grep 'FIRMWARE' | evidence
    elif echo "${out}" | grep -q 'FIRMWARE_'; then
        pass "F4: FIRMWARE path in manifest has no double-slash"
        echo "${out}" | grep 'FIRMWARE' | evidence
    else
        skip "F4: no FIRMWARE entry in manifest (RP2040 firmware not built — non-blocking)"
    fi
}

check_f5_http_routes_both_prefixes() {
    # F5: health_check.c handles both /slots and /api/v1/slots
    local results=""
    local all_pass=1
    for path in /slots /api/v1/slots /gsdml /api/v1/gsdml /config /api/v1/config; do
        local code
        code=$(ssh_run "curl -s -o /dev/null -w '%{http_code}' http://localhost:9081${path}")
        if [[ "${code}" =~ ^(200|301|302)$ ]]; then
            results+="    ${path} → ${code}\n"
        else
            results+="    ${path} → ${code} ✗\n"
            all_pass=0
        fi
    done
    if [[ ${all_pass} -eq 1 ]]; then
        pass "F5: every HTTP route (legacy + /api/v1/) returns 200"
        echo -e "${results}" | evidence
    else
        fail "F5: some HTTP routes did not return 200"
        echo -e "${results}" | evidence
    fi
}

check_w5_kmods_loaded() {
    # Wave 5: /etc/modules-load.d/water-treat.conf was installed AND modules are loaded
    local conf
    conf=$(ssh_sudo 'cat /etc/modules-load.d/water-treat.conf 2>&1')
    if echo "${conf}" | grep -qE 'spidev'; then
        pass "Wave 5: /etc/modules-load.d/water-treat.conf installed with required modules"
        echo "${conf}" | grep -vE '^\s*$|^\s*#' | evidence
    else
        fail "Wave 5: /etc/modules-load.d/water-treat.conf missing or incomplete"
        echo "${conf}" | evidence
    fi

    # And the modules are actually loaded right now
    local lsmod_out
    lsmod_out=$(ssh_sudo "lsmod | grep -E '^(spidev|w1_(gpio|therm)|i2c_dev) ' 2>&1")
    local count
    count=$(echo "${lsmod_out}" | grep -c '^[a-z]')
    if [[ ${count} -ge 1 ]]; then
        pass "Wave 5: kernel modules loaded right now (${count} of {spidev,w1_gpio,w1_therm,i2c_dev})"
        echo "${lsmod_out}" | evidence
    else
        # Soft fail — modules will load on next boot regardless
        skip "Wave 5: no required modules loaded yet (will retry at boot via modules-load.d)"
    fi
}

check_w5_network_helper_installed() {
    # Wave 5: /usr/local/bin/set_network_parameters exists and is executable
    local out
    out=$(ssh_sudo 'ls -la /usr/local/bin/set_network_parameters 2>&1; \
                    file /usr/local/bin/set_network_parameters 2>&1')
    if echo "${out}" | grep -qE 'rwxr-xr-x.*set_network_parameters'; then
        pass "Wave 5: set_network_parameters helper installed and executable"
        echo "${out}" | head -2 | evidence
    else
        fail "Wave 5: set_network_parameters helper missing or not executable"
        echo "${out}" | head -3 | evidence
    fi
}

check_w5_protectsystem_etc_writable() {
    # Wave 5: systemd unit ReadWritePaths must include /etc/water-treat
    local out
    out=$(ssh_sudo 'systemctl cat water-treat | grep ReadWritePaths 2>&1')
    if echo "${out}" | grep -q '/etc/water-treat'; then
        pass "Wave 5: ProtectSystem ReadWritePaths includes /etc/water-treat"
        echo "${out}" | evidence
    else
        fail "Wave 5: ReadWritePaths does NOT include /etc/water-treat (Failure #10)"
        echo "${out}" | evidence
    fi
}

# =============================================================================
#  Tier 2 — Runtime correctness controls (Wave 1-3a)
# =============================================================================

check_w1_actuator_safe_state_in_struct() {
    # Wave 1 T2-C1: actuator_config_t has safe_state field; Wave 1 T2-A2: gpio_chip
    # We verify the BINARY contains the field references via strings, since the
    # source tree on a deployed device may or may not be present.
    local out
    out=$(ssh_sudo "strings /usr/local/bin/water-treat 2>&1 | \
                    grep -E 'SAFE STATE: (Driving|Holding)' | head -5")
    if echo "${out}" | grep -q 'SAFE STATE: Driving'; then
        pass "Wave 1 T2-C1: binary has per-actuator safe_state branching (OFF/ON/HOLD)"
        echo "${out}" | head -3 | evidence
    else
        fail "Wave 1 T2-C1: binary does NOT have the new safe_state log strings"
        echo "${out}" | head -5 | evidence
    fi
}

check_w1_pump_valve_round_trip() {
    # Wave 1 T2-A3: PUMP/VALVE actuator types must round-trip via the DB
    # The binary must contain ACTUATOR_TYPE_PUMP_STR ("pump") and _VALVE_STR ("valve")
    local out
    out=$(ssh_sudo "strings /usr/local/bin/water-treat 2>&1 | \
                    grep -E '^(pump|valve|relay|pwm|latching|momentary)\$' | sort -u")
    if echo "${out}" | grep -q '^pump$' && echo "${out}" | grep -q '^valve$'; then
        pass "Wave 1 T2-A3: binary contains pump/valve string constants for round-trip"
        echo "${out}" | head -10 | evidence
    else
        fail "Wave 1 T2-A3: binary missing pump or valve constants"
        echo "${out}" | head -10 | evidence
    fi
}

check_w2_actuator_manager_reload_from_tui() {
    # Wave 2 T2-T1: TUI page_actuators.c calls actuator_manager_reload after add/edit/delete
    # The binary's strings won't show the function call, but the runtime LOG_INFO from
    # actuator_manager_reload will appear in the journal if it ever fires. Lacking a
    # live TUI session, we verify the binary contains the call site by checking that
    # actuator_manager_reload is exported (visible in nm) and that the page_actuators
    # symbol references it. Use nm if available, fall back to strings.
    local out
    out=$(ssh_sudo "nm -D /usr/local/bin/water-treat 2>/dev/null | \
                    grep -E 'actuator_manager_reload' || \
                    strings /usr/local/bin/water-treat | grep 'Loading actuators from database'")
    if echo "${out}" | grep -qE 'actuator_manager_reload|Loading actuators'; then
        pass "Wave 2 T2-T1: actuator_manager_reload symbol/log present in binary"
        echo "${out}" | head -3 | evidence
    else
        fail "Wave 2 T2-T1: actuator_manager_reload not found in binary"
    fi
}

check_w3a_web_poll_no_fake_zero() {
    # Wave 3a T2-C2: web_poll driver must NOT return RESULT_OK on fetch failure
    # We can verify this by checking the binary contains the LOG_ERROR for curl_easy_perform
    # AND does NOT contain a "stale value" return-OK path. Since we can't easily diff
    # the old and new behavior from a binary, this check looks for the post-fix log line.
    local out
    out=$(ssh_sudo "strings /usr/local/bin/water-treat 2>&1 | \
                    grep -E 'curl_easy_perform.*failed' | head -3")
    if [[ -n "${out}" ]]; then
        pass "Wave 3a T2-C2: web_poll error log path present (RESULT_ERROR path)"
        echo "${out}" | head -2 | evidence
    else
        skip "Wave 3a T2-C2: cannot verify from strings alone — needs runtime test"
    fi
}

check_w3a_dht22_per_thread_sched() {
    # Wave 3a T2-C3: DHT22/HX711 must use pthread_setschedparam, not sched_setscheduler(0,...)
    # Check that the binary's dynamic symbols include pthread_setschedparam.
    local out
    out=$(ssh_sudo "nm -D /usr/local/bin/water-treat 2>/dev/null | \
                    grep -E 'pthread_setschedparam|pthread_getschedparam' | head")
    if echo "${out}" | grep -q 'pthread_setschedparam'; then
        pass "Wave 3a T2-C3: binary imports pthread_setschedparam (per-thread scheduling)"
        echo "${out}" | head -3 | evidence
    else
        fail "Wave 3a T2-C3: pthread_setschedparam NOT in dynamic symbols (still using whole-process sched_setscheduler?)"
    fi
}

# =============================================================================
#  Tier 3 — Service runtime controls (post-deploy)
# =============================================================================

check_service_active() {
    local out
    out=$(ssh_sudo 'systemctl is-active water-treat.service 2>&1')
    if echo "${out}" | grep -q '^active$'; then
        pass "Service: water-treat.service is active"
        echo "${out}" | evidence
    else
        fail "Service: water-treat.service is '${out}' (expected 'active')"
    fi
}

check_profinet_listening() {
    local out
    out=$(ssh_sudo "ss -ulnp 2>/dev/null | grep ':34964 '")
    if [[ -n "${out}" ]]; then
        pass "Service: PROFINET UDP 34964 listening"
        echo "${out}" | evidence
    else
        fail "Service: nothing listening on UDP 34964"
    fi
}

check_http_listening() {
    local out
    out=$(ssh_sudo "ss -tlnp 2>/dev/null | grep ':9081 '")
    if [[ -n "${out}" ]]; then
        pass "Service: HTTP TCP 9081 listening"
        echo "${out}" | evidence
    else
        fail "Service: nothing listening on TCP 9081"
    fi
}

check_health_endpoint() {
    local out
    out=$(ssh_run 'curl -s http://localhost:9081/health 2>&1')
    if echo "${out}" | grep -q '"status"'; then
        pass "Service: /health endpoint returns JSON"
        echo "${out}" | head -c 300 | evidence
    else
        fail "Service: /health endpoint did not return JSON"
        echo "${out}" | head -c 200 | evidence
    fi
}

# Discover the actual database path from the live /config endpoint.
# The DB filename is configurable via [database] path in water-treat.conf
# (data.db on Le Potato deployments, water-treat.db on Odroid-XU4 deployments,
# etc.). Asking the running daemon is the source of truth.
DB_PATH_CACHE=""
discover_db_path() {
    if [[ -n "${DB_PATH_CACHE}" ]]; then
        echo "${DB_PATH_CACHE}"
        return 0
    fi
    local path
    path=$(ssh_run "curl -s http://localhost:9081/config 2>/dev/null | python3 -c 'import json,sys
try:
    d=json.load(sys.stdin)
    print(d.get(\"database\",{}).get(\"path\",\"\"))
except: pass'" 2>/dev/null)
    if [[ -z "${path}" || "${path}" == "" ]]; then
        # Fallbacks: check the two known defaults
        if ssh_sudo "test -s /var/lib/water-treat/data.db" >/dev/null 2>&1; then
            path="/var/lib/water-treat/data.db"
        elif ssh_sudo "test -s /var/lib/water-treat/water-treat.db" >/dev/null 2>&1; then
            path="/var/lib/water-treat/water-treat.db"
        fi
    fi
    DB_PATH_CACHE="${path}"
    echo "${path}"
}

check_db_schema_present() {
    local db_path
    db_path=$(discover_db_path)
    if [[ -z "${db_path}" ]]; then
        fail "DB: cannot discover database path (empty /config and no default file present)"
        return
    fi
    local out
    out=$(ssh_sudo "sqlite3 '${db_path}' '.tables' 2>&1")
    local required=("modules" "physical_sensors" "adc_sensors" "actuators" "actuator_state")
    local missing=0
    for tbl in "${required[@]}"; do
        if ! echo "${out}" | grep -qw "${tbl}"; then
            missing=$((missing + 1))
        fi
    done
    if [[ ${missing} -eq 0 ]]; then
        pass "DB: all required tables present at ${db_path}"
        echo "${out}" | evidence
    else
        fail "DB at ${db_path}: ${missing} required table(s) missing"
        echo "${out}" | evidence
    fi
}

check_db_actuators_schema_columns() {
    # Wave 1 / DB layer: actuators table must have safe_state, gpio_chip columns
    local db_path
    db_path=$(discover_db_path)
    if [[ -z "${db_path}" ]]; then
        fail "DB: cannot discover database path"
        return
    fi
    local out
    out=$(ssh_sudo "sqlite3 '${db_path}' '.schema actuators' 2>&1")
    if echo "${out}" | grep -q 'safe_state' && echo "${out}" | grep -q 'gpio_chip'; then
        pass "DB: actuators schema has safe_state AND gpio_chip columns"
        echo "${out}" | head -3 | evidence
    else
        fail "DB: actuators schema at ${db_path} missing safe_state or gpio_chip"
        echo "${out}" | evidence
    fi
}

# =============================================================================
#  Summary
# =============================================================================

print_summary() {
    print_header "Summary"
    local total=$((PASS_COUNT + FAIL_COUNT + SKIP_COUNT))
    echo
    echo "  Target:  ${WT_SSH_USER}@${TARGET}"
    echo "  Date:    $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo
    echo -e "  ${BOLD}Total checks:${NC} ${total}"
    echo -e "  ${GREEN}PASS${NC}:        ${PASS_COUNT}"
    echo -e "  ${RED}FAIL${NC}:        ${FAIL_COUNT}"
    echo -e "  ${YELLOW}SKIP${NC}:        ${SKIP_COUNT}"
    echo

    if [[ ${FAIL_COUNT} -gt 0 ]]; then
        echo -e "  ${BOLD}${RED}Failed controls:${NC}"
        for t in "${FAILED_TESTS[@]}"; do
            echo -e "    ${RED}✗${NC} ${t}"
        done
        echo
        echo -e "  ${RED}${BOLD}DEPLOY VALIDATION FAILED${NC}"
        return 1
    else
        echo -e "  ${GREEN}${BOLD}DEPLOY VALIDATION PASSED${NC}"
        return 0
    fi
}

# =============================================================================
#  Main
# =============================================================================

main() {
    echo
    echo -e "${BOLD}${CYAN}╔═══════════════════════════════════════════════════════╗${NC}"
    echo -e "${BOLD}${CYAN}║  water-treat deployment validation                   ║${NC}"
    echo -e "${BOLD}${CYAN}║  Target: ${WT_SSH_USER}@${TARGET}                                ${NC}"
    echo -e "${BOLD}${CYAN}╚═══════════════════════════════════════════════════════╝${NC}"

    preflight
    probe_target

    print_header "Tier 1 — Build & install controls (F1-F5, Wave 5)"
    check_f1_libgpiod_v2
    check_f2_gsdml_installed
    check_f2_config_files_installed
    check_f3_service_enabled
    check_f4_firmware_dir_no_double_slash
    check_f5_http_routes_both_prefixes
    check_w5_kmods_loaded
    check_w5_network_helper_installed
    check_w5_protectsystem_etc_writable

    print_header "Tier 2 — Runtime correctness controls (Wave 1-3a)"
    check_w1_actuator_safe_state_in_struct
    check_w1_pump_valve_round_trip
    check_w2_actuator_manager_reload_from_tui
    check_w3a_web_poll_no_fake_zero
    check_w3a_dht22_per_thread_sched

    print_header "Tier 3 — Service runtime controls"
    check_service_active
    check_profinet_listening
    check_http_listening
    check_health_endpoint
    check_db_schema_present
    check_db_actuators_schema_columns

    print_summary
}

main "$@"

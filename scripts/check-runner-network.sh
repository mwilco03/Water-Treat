#!/bin/bash
# =============================================================================
# GitHub Actions Self-Hosted Runner - Network Diagnostics
# =============================================================================
# Validates network connectivity, DNS, TLS, and endpoint health required by
# the GitHub Actions runner before running ./config.sh --check.
#
# Reference: https://github.com/actions/runner/blob/main/docs/checks/network.md
#
# Usage:
#   bash scripts/check-runner-network.sh
#   bash scripts/check-runner-network.sh --verbose
#   bash scripts/check-runner-network.sh --fix       # attempt auto-fixes
#
# Exit codes:
#   0  All checks passed
#   1  One or more checks failed
# =============================================================================

set -uo pipefail

# ---------------------------------------------------------------------------
# Globals
# ---------------------------------------------------------------------------
VERBOSE=false
FIX_MODE=false
PASS=0
FAIL=0
WARN=0
SKIP=0

readonly RED='\033[0;31m'
readonly GREEN='\033[0;32m'
readonly YELLOW='\033[1;33m'
readonly BLUE='\033[0;34m'
readonly CYAN='\033[0;36m'
readonly NC='\033[0m'

# GitHub Actions runner required endpoints (github.com, not GHES)
readonly -a HEALTH_ENDPOINTS=(
    "https://api.github.com/"
    "https://vstoken.actions.githubusercontent.com/_apis/health"
    "https://pipelines.actions.githubusercontent.com/_apis/health"
    "https://results-receiver.actions.githubusercontent.com/health"
)

# Domains the runner must reach (wildcard parents)
readonly -a REQUIRED_DOMAINS=(
    "github.com"
    "api.github.com"
    "vstoken.actions.githubusercontent.com"
    "pipelines.actions.githubusercontent.com"
    "results-receiver.actions.githubusercontent.com"
    "codeload.github.com"
    "objects.githubusercontent.com"
)

# ---------------------------------------------------------------------------
# Output helpers
# ---------------------------------------------------------------------------
pass()  { ((PASS++));  echo -e "  ${GREEN}[PASS]${NC} $1"; }
fail()  { ((FAIL++));  echo -e "  ${RED}[FAIL]${NC} $1"; }
warn()  { ((WARN++));  echo -e "  ${YELLOW}[WARN]${NC} $1"; }
skip()  { ((SKIP++));  echo -e "  ${CYAN}[SKIP]${NC} $1"; }
info()  { echo -e "  ${BLUE}[INFO]${NC} $1"; }
detail(){ $VERBOSE && echo -e "        $1"; }
header(){ echo ""; echo -e "${BLUE}== $1 ==${NC}"; }

# ---------------------------------------------------------------------------
# Parse args
# ---------------------------------------------------------------------------
while [[ $# -gt 0 ]]; do
    case "$1" in
        --verbose|-v) VERBOSE=true; shift ;;
        --fix|-f)     FIX_MODE=true; shift ;;
        --help|-h)
            echo "Usage: $0 [--verbose] [--fix]"
            echo "  --verbose   Show detailed output for each check"
            echo "  --fix       Attempt automatic fixes for failures"
            exit 0
            ;;
        *) echo "Unknown option: $1"; exit 1 ;;
    esac
done

# ---------------------------------------------------------------------------
# 1. Prerequisites
# ---------------------------------------------------------------------------
header "Prerequisites"

for tool in curl openssl nslookup date; do
    if command -v "$tool" &>/dev/null; then
        pass "$tool available ($(command -v "$tool"))"
    else
        case "$tool" in
            nslookup)
                warn "$tool not found (DNS checks will be skipped)"
                ;;
            *)
                fail "$tool not found (required)"
                ;;
        esac
    fi
done

# Check architecture (ARM32 has known .NET TLS issues)
arch=$(uname -m)
if [[ "$arch" == "armv7l" || "$arch" == "armhf" ]]; then
    warn "ARM32 detected ($arch) - .NET 6 has known TLS bugs on this arch"
    info "Ensure DOTNET_SYSTEM_NET_HTTP_USESOCKETSHTTPHANDLER=0 is set in .env"
else
    pass "Architecture: $arch"
fi

# ---------------------------------------------------------------------------
# 2. DNS Resolution
# ---------------------------------------------------------------------------
header "DNS Resolution"

if command -v nslookup &>/dev/null; then
    for domain in "${REQUIRED_DOMAINS[@]}"; do
        dns_out=$(nslookup "$domain" 2>&1)
        if echo "$dns_out" | grep -q "Address:" | grep -v "#53" &>/dev/null; then
            # nslookup always shows "Address:" for the DNS server itself
            # Check for actual resolution by looking for a non-server address
            ip=$(echo "$dns_out" | awk '/^Address:/ && !/#53/ {print $2}' | head -1)
            if [[ -z "$ip" ]]; then
                # Fallback: check if Name: line exists
                if echo "$dns_out" | grep -qi "name:"; then
                    pass "$domain resolves"
                    detail "$(echo "$dns_out" | grep -i 'address' | tail -1)"
                else
                    fail "$domain - DNS resolution failed"
                    detail "$dns_out"
                fi
            else
                pass "$domain -> $ip"
            fi
        else
            # Try getent as fallback
            ip=$(getent hosts "$domain" 2>/dev/null | awk '{print $1}' | head -1)
            if [[ -n "$ip" ]]; then
                pass "$domain -> $ip (via getent)"
            else
                fail "$domain - DNS resolution failed"
                detail "$dns_out"
            fi
        fi
    done
else
    # Fallback to getent
    for domain in "${REQUIRED_DOMAINS[@]}"; do
        ip=$(getent hosts "$domain" 2>/dev/null | awk '{print $1}' | head -1)
        if [[ -n "$ip" ]]; then
            pass "$domain -> $ip"
        else
            fail "$domain - cannot resolve"
        fi
    done
fi

# ---------------------------------------------------------------------------
# 3. TLS / Certificate Validation
# ---------------------------------------------------------------------------
header "TLS Certificate Validation"

if command -v openssl &>/dev/null; then
    for domain in github.com api.github.com vstoken.actions.githubusercontent.com pipelines.actions.githubusercontent.com; do
        tls_out=$(echo | openssl s_client -connect "${domain}:443" -servername "$domain" 2>&1)
        verify_code=$(echo "$tls_out" | grep "Verify return code:" | sed 's/.*Verify return code: //')

        if [[ -z "$verify_code" ]]; then
            fail "$domain - could not connect (openssl s_client failed)"
            continue
        elif echo "$verify_code" | grep -q "^0 "; then
            pass "$domain - certificate valid ($verify_code)"

            # Extract cert dates for extra detail
            if $VERBOSE; then
                not_before=$(echo "$tls_out" | openssl x509 -noout -startdate 2>/dev/null | cut -d= -f2)
                not_after=$(echo "$tls_out" | openssl x509 -noout -enddate 2>/dev/null | cut -d= -f2)
                detail "  Valid: $not_before -> $not_after"
            fi
        else
            fail "$domain - certificate INVALID: $verify_code"
            detail "Run: echo | openssl s_client -connect ${domain}:443 -servername $domain 2>&1 | head -30"

            # Check specific failure reasons
            if echo "$verify_code" | grep -qi "expired\|NotTimeValid"; then
                info "Certificate appears expired - check system clock"
                info "Current time: $(date -u)"
            fi
        fi

        # Check TLS version
        tls_ver=$(echo "$tls_out" | grep "Protocol  :" | awk '{print $3}')
        if [[ -n "$tls_ver" ]]; then
            case "$tls_ver" in
                TLSv1.2|TLSv1.3)
                    detail "TLS version: $tls_ver (OK)"
                    ;;
                *)
                    warn "$domain using $tls_ver (runner requires TLS 1.2+)"
                    ;;
            esac
        fi
    done
else
    skip "openssl not available - cannot validate certificates"
fi

# ---------------------------------------------------------------------------
# 4. System Clock
# ---------------------------------------------------------------------------
header "System Clock"

local_time=$(date -u '+%Y-%m-%d %H:%M:%S UTC')
local_epoch=$(date +%s)
info "System time: $local_time"

# Compare against an HTTP Date header to detect clock skew
if command -v curl &>/dev/null; then
    http_date=$(curl -sI --connect-timeout 5 --max-time 10 https://api.github.com/ 2>/dev/null \
        | grep -i "^date:" | head -1 | sed 's/^[Dd]ate: //' | tr -d '\r')
    if [[ -n "$http_date" ]]; then
        # Parse HTTP date to epoch
        remote_epoch=$(date -d "$http_date" +%s 2>/dev/null)
        if [[ -n "$remote_epoch" ]]; then
            skew=$(( local_epoch - remote_epoch ))
            abs_skew=${skew#-}  # absolute value
            if [[ $abs_skew -lt 30 ]]; then
                pass "Clock skew: ${skew}s (within tolerance)"
            elif [[ $abs_skew -lt 300 ]]; then
                warn "Clock skew: ${skew}s (may cause TLS issues)"
            else
                fail "Clock skew: ${skew}s (will cause certificate validation failures)"
                if $FIX_MODE; then
                    info "Attempting ntpdate sync..."
                    if command -v ntpdate &>/dev/null; then
                        sudo ntpdate -u pool.ntp.org 2>&1 && pass "Clock synced via ntpdate" || fail "ntpdate sync failed"
                    elif command -v ntpd &>/dev/null; then
                        sudo ntpd -gq 2>&1 && pass "Clock synced via ntpd" || fail "ntpd sync failed"
                    else
                        fail "No NTP client available (install ntpsec-ntpdate)"
                    fi
                fi
            fi
        else
            warn "Could not parse remote date header: $http_date"
        fi
    else
        warn "Could not fetch Date header from api.github.com"
    fi
fi

# ---------------------------------------------------------------------------
# 5. Endpoint Health Checks
# ---------------------------------------------------------------------------
header "GitHub Actions Endpoint Health"

if command -v curl &>/dev/null; then
    for url in "${HEALTH_ENDPOINTS[@]}"; do
        http_code=$(curl -s -o /dev/null -w '%{http_code}' \
            --connect-timeout 10 --max-time 15 \
            -H "User-Agent: github-actions-runner-check/1.0" \
            "$url" 2>/dev/null)
        curl_rc=$?

        if [[ $curl_rc -ne 0 ]]; then
            fail "$url - curl error $curl_rc (connection failed)"
            case $curl_rc in
                6)  detail "DNS resolution failed" ;;
                7)  detail "Connection refused" ;;
                28) detail "Connection timed out" ;;
                35) detail "TLS/SSL handshake failed" ;;
                60) detail "Certificate verification failed" ;;
                *)  detail "curl exit code: $curl_rc" ;;
            esac
        elif [[ "$http_code" -ge 200 && "$http_code" -lt 400 ]]; then
            pass "$url -> HTTP $http_code"
        elif [[ "$http_code" == "403" ]]; then
            # api.github.com rate-limits unauthenticated requests
            warn "$url -> HTTP 403 (rate-limited, but reachable)"
        else
            fail "$url -> HTTP $http_code"
        fi
    done
else
    skip "curl not available - cannot test endpoints"
fi

# ---------------------------------------------------------------------------
# 6. HTTP Methods (POST/PUT must not be blocked)
# ---------------------------------------------------------------------------
header "HTTP Method Accessibility"

if command -v curl &>/dev/null; then
    # The runner uses POST and PUT for log uploads and status updates.
    # Some proxies/firewalls block non-GET methods.
    for method in POST PUT; do
        # We send to api.github.com - it will return 404 or 422 but that's fine,
        # we just need to confirm the method isn't blocked at the network layer.
        http_code=$(curl -s -o /dev/null -w '%{http_code}' \
            --connect-timeout 10 --max-time 15 \
            -X "$method" \
            -H "User-Agent: github-actions-runner-check/1.0" \
            -H "Content-Type: application/json" \
            -d '{}' \
            "https://api.github.com/" 2>/dev/null)
        curl_rc=$?

        if [[ $curl_rc -ne 0 ]]; then
            fail "$method to api.github.com - blocked (curl error $curl_rc)"
        elif [[ "$http_code" == "000" ]]; then
            fail "$method to api.github.com - no response (proxy blocking?)"
        else
            # Any HTTP response means the method got through
            pass "$method to api.github.com -> HTTP $http_code (method not blocked)"
        fi
    done
else
    skip "curl not available - cannot test HTTP methods"
fi

# ---------------------------------------------------------------------------
# 7. .NET TLS Backend (ARM32-specific)
# ---------------------------------------------------------------------------
header ".NET Runtime Environment"

runner_dir=""
for candidate in ~/actions-runner ./actions-runner /opt/actions-runner; do
    if [[ -d "$candidate" ]]; then
        runner_dir="$candidate"
        break
    fi
done

if [[ -n "$runner_dir" ]]; then
    pass "Runner directory found: $runner_dir"

    # Check .env file for TLS workaround
    env_file="$runner_dir/.env"
    if [[ -f "$env_file" ]]; then
        if grep -q "DOTNET_SYSTEM_NET_HTTP_USESOCKETSHTTPHANDLER=0" "$env_file"; then
            pass ".env has DOTNET_SYSTEM_NET_HTTP_USESOCKETSHTTPHANDLER=0 (uses libcurl)"
        else
            if [[ "$arch" == "armv7l" || "$arch" == "armhf" ]]; then
                fail ".env missing DOTNET_SYSTEM_NET_HTTP_USESOCKETSHTTPHANDLER=0"
                info "ARM32 needs this to work around .NET 6 TLS bugs"
                if $FIX_MODE; then
                    echo "DOTNET_SYSTEM_NET_HTTP_USESOCKETSHTTPHANDLER=0" >> "$env_file"
                    pass "Added DOTNET_SYSTEM_NET_HTTP_USESOCKETSHTTPHANDLER=0 to .env"
                fi
            else
                info ".env does not set DOTNET_SYSTEM_NET_HTTP_USESOCKETSHTTPHANDLER (OK on $arch)"
            fi
        fi
    else
        if [[ "$arch" == "armv7l" || "$arch" == "armhf" ]]; then
            warn ".env file not found at $env_file"
            if $FIX_MODE; then
                echo "DOTNET_SYSTEM_NET_HTTP_USESOCKETSHTTPHANDLER=0" > "$env_file"
                pass "Created $env_file with libcurl TLS backend"
            fi
        else
            info ".env not found (optional on $arch)"
        fi
    fi

    # Check for libcurl (needed by .NET fallback HTTP handler)
    if ldconfig -p 2>/dev/null | grep -q libcurl; then
        libcurl_path=$(ldconfig -p 2>/dev/null | grep libcurl | head -1 | awk '{print $NF}')
        pass "libcurl available: $libcurl_path"
    elif [[ -f /usr/lib/arm-linux-gnueabihf/libcurl.so.4 ]] || \
         [[ -f /usr/lib/arm-linux-gnueabihf/libcurl.so.4t64 ]]; then
        pass "libcurl available (ARM multiarch path)"
    else
        if [[ "$arch" == "armv7l" || "$arch" == "armhf" ]]; then
            fail "libcurl not found (required for .NET TLS fallback on ARM32)"
            if $FIX_MODE; then
                info "Attempting to install libcurl..."
                if apt-cache show libcurl4 &>/dev/null 2>&1; then
                    sudo apt-get install -y libcurl4 && pass "Installed libcurl4" || fail "Install failed"
                elif apt-cache show libcurl4t64 &>/dev/null 2>&1; then
                    sudo apt-get install -y libcurl4t64 && pass "Installed libcurl4t64" || fail "Install failed"
                fi
            fi
        else
            warn "libcurl not found in ldconfig cache (may still work)"
        fi
    fi
else
    skip "No runner directory found (checked ~/actions-runner, ./actions-runner, /opt/actions-runner)"
fi

# ---------------------------------------------------------------------------
# 8. Runner Runtime Dependencies
# ---------------------------------------------------------------------------
header "Runner Runtime Dependencies (.NET 6)"

# These are the libraries the runner's .NET 6 runtime needs
declare -A RUNNER_LIBS=(
    ["libssl"]="TLS/SSL support"
    ["libicu"]="Unicode / globalization"
    ["libcurl"]="HTTP client (fallback TLS backend)"
    ["liblttng-ust"]="Tracing (optional)"
    ["libkrb5"]="Kerberos auth (optional)"
    ["libz"]="Compression (zlib)"
)

if command -v ldconfig &>/dev/null; then
    for lib in "${!RUNNER_LIBS[@]}"; do
        desc="${RUNNER_LIBS[$lib]}"
        if ldconfig -p 2>/dev/null | grep -q "$lib"; then
            so_path=$(ldconfig -p 2>/dev/null | grep "$lib" | head -1 | awk '{print $NF}')
            pass "$lib - $desc ($so_path)"
        else
            case "$lib" in
                liblttng-ust|libkrb5)
                    warn "$lib not found - $desc"
                    ;;
                *)
                    fail "$lib not found - $desc (required)"
                    ;;
            esac
        fi
    done
elif [[ -d /usr/lib ]]; then
    warn "ldconfig not in PATH (trying /sbin/ldconfig)"
    export PATH="$PATH:/sbin:/usr/sbin"
    if command -v ldconfig &>/dev/null; then
        info "Found ldconfig at $(command -v ldconfig), re-running checks..."
        for lib in "${!RUNNER_LIBS[@]}"; do
            desc="${RUNNER_LIBS[$lib]}"
            if ldconfig -p 2>/dev/null | grep -q "$lib"; then
                pass "$lib - $desc"
            else
                case "$lib" in
                    liblttng-ust|libkrb5) warn "$lib not found - $desc" ;;
                    *) fail "$lib not found - $desc (required)" ;;
                esac
            fi
        done
    else
        skip "ldconfig not available - cannot verify shared libraries"
    fi
fi

# ---------------------------------------------------------------------------
# 9. Proxy Detection
# ---------------------------------------------------------------------------
header "Proxy Configuration"

proxy_found=false
for var in http_proxy https_proxy HTTP_PROXY HTTPS_PROXY no_proxy NO_PROXY; do
    val="${!var:-}"
    if [[ -n "$val" ]]; then
        info "$var=$val"
        proxy_found=true
    fi
done

if $proxy_found; then
    warn "Proxy environment detected - ensure it allows HTTPS to *.github.com and *.actions.githubusercontent.com"
    warn "Runner docs: proxies may block POST/PUT or modify headers"
else
    pass "No proxy environment variables set"
fi

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
header "Summary"

total=$((PASS + FAIL + WARN + SKIP))
echo ""
echo -e "  ${GREEN}Passed:  $PASS${NC}"
echo -e "  ${RED}Failed:  $FAIL${NC}"
echo -e "  ${YELLOW}Warnings: $WARN${NC}"
echo -e "  ${CYAN}Skipped: $SKIP${NC}"
echo -e "  Total:   $total"
echo ""

if [[ $FAIL -gt 0 ]]; then
    echo -e "${RED}Some checks failed. Fix the issues above before running ./config.sh --check${NC}"
    if ! $FIX_MODE; then
        echo -e "Run with ${CYAN}--fix${NC} to attempt automatic repairs."
    fi
    exit 1
else
    if [[ $WARN -gt 0 ]]; then
        echo -e "${YELLOW}All critical checks passed but there are warnings to review.${NC}"
    else
        echo -e "${GREEN}All checks passed. Runner should be able to connect.${NC}"
    fi
    exit 0
fi

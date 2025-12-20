#!/usr/bin/env bash
#
# uninstall.sh - Remove Water-Treat RTU installation
#
# What: Removes binary, service, and optionally data/config
# Why: Clean removal for re-installation or decommissioning
#
# Usage:
#   sudo ./scripts/uninstall.sh           # Keep config, data, logs, user
#   sudo ./scripts/uninstall.sh --purge   # Remove everything
#   sudo ./scripts/uninstall.sh --dry-run # Show what would be removed
#
# By default, config/data/logs/user are KEPT to preserve settings.
# Use --purge to remove everything.
#

set -uo pipefail

# ------------------------------------------------------------------------------
# Source shared environment
# ------------------------------------------------------------------------------
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=env.sh
source "${SCRIPT_DIR}/env.sh"

# ------------------------------------------------------------------------------
# Options
# ------------------------------------------------------------------------------
# What: Parse command-line flags
# Why: Different removal levels (keep data vs purge everything)
# Edge cases: Unknown flags are warnings
# User impact: Control over what gets removed
#

PURGE=false
DRY_RUN=false

parse_args() {
    for arg in "$@"; do
        case "${arg}" in
            --purge|-p)
                PURGE=true
                ;;
            --dry-run|-n)
                DRY_RUN=true
                ;;
            --help|-h)
                cat <<EOF
Usage: $0 [OPTIONS]

Options:
  --purge, -p    Remove ALL data, configuration, logs, and user
  --dry-run, -n  Show what would be removed without doing it
  --help, -h     Show this help message

Default behavior (no flags):
  - Stops and removes systemd service
  - Removes binary from /usr/local/bin
  - KEEPS configuration in /etc/
  - KEEPS data in /var/lib/
  - KEEPS logs in /var/log/
  - KEEPS service user

This preserves data for re-installation.
Use --purge to remove everything.

Examples:
  sudo $0               # Remove service and binary, keep data
  sudo $0 --dry-run     # Preview what would be removed
  sudo $0 --purge       # Remove everything including data
EOF
                exit 0
                ;;
            *)
                warn "Unknown option: ${arg}"
                ;;
        esac
    done
}

# ------------------------------------------------------------------------------
# Root Check
# ------------------------------------------------------------------------------

check_root() {
    if [[ $EUID -ne 0 ]]; then
        breaking "Root privileges required"
        echo ""
        error "Please run: sudo $0"
        return 1
    fi
    return 0
}

# ------------------------------------------------------------------------------
# Manifest Discovery
# ------------------------------------------------------------------------------
# What: Find and read the install manifest
# Why: Manifest tells us what was installed and where
# Edge cases: Missing manifest - try to discover from system state
# User impact: If manifest missing, we'll try to figure it out
#

declare -A MANIFEST=()

find_manifest() {
    # Known manifest locations (based on common project names)
    local -a possible_paths=(
        "/etc/profinet-monitor/.install-manifest"
        "/etc/water-treat/.install-manifest"
    )

    # Also search /etc for any manifest files
    while IFS= read -r -d '' manifest; do
        possible_paths+=("${manifest}")
    done < <(find /etc -maxdepth 2 -name ".install-manifest" -print0 2>/dev/null)

    for path in "${possible_paths[@]}"; do
        if [[ -f "${path}" ]]; then
            echo "${path}"
            return 0
        fi
    done

    return 1
}

read_manifest() {
    local manifest_file="$1"

    info "Reading manifest: ${manifest_file}"

    while IFS='=' read -r key value; do
        # Skip comments and empty lines
        [[ "${key}" =~ ^#.*$ ]] && continue
        [[ -z "${key}" ]] && continue
        # Remove any leading/trailing whitespace
        key="${key// /}"
        MANIFEST["${key}"]="${value}"
    done < "${manifest_file}"

    MANIFEST[MANIFEST_FILE]="${manifest_file}"

    detail "Project: ${MANIFEST[PROJECT_NAME]:-unknown}"
    detail "Binary: ${MANIFEST[BINARY]:-unknown}"
    detail "Service: ${MANIFEST[SERVICE_FILE]:-unknown}"
}

# ------------------------------------------------------------------------------
# Fallback Discovery
# ------------------------------------------------------------------------------
# What: If no manifest, try to find installation from system state
# Why: Handle cases where manifest was deleted or corrupted
# Edge cases: Nothing found - that's okay, nothing to uninstall
# User impact: We try our best to find what to remove
#

discover_from_system() {
    info "No manifest found, discovering from system..."

    # Look for our service files
    local -a service_patterns=("profinet-monitor" "water-treat")

    for name in "${service_patterns[@]}"; do
        local service_file="/etc/systemd/system/${name}.service"
        if [[ -f "${service_file}" ]]; then
            MANIFEST[PROJECT_NAME]="${name}"
            MANIFEST[SERVICE_FILE]="${service_file}"

            # Try to extract binary path from service file
            local exec_start
            exec_start="$(grep "^ExecStart=" "${service_file}" 2>/dev/null | cut -d= -f2 | awk '{print $1}')"
            if [[ -n "${exec_start}" && -x "${exec_start}" ]]; then
                MANIFEST[BINARY]="${exec_start}"
            elif [[ -x "/usr/local/bin/${name}" ]]; then
                MANIFEST[BINARY]="/usr/local/bin/${name}"
            fi

            # Infer directories
            [[ -d "/etc/${name}" ]] && MANIFEST[ETC_DIR]="/etc/${name}"
            [[ -d "/var/lib/${name}" ]] && MANIFEST[VAR_DIR]="/var/lib/${name}"
            [[ -d "/var/log/${name}" ]] && MANIFEST[LOG_DIR]="/var/log/${name}"

            # Check for user
            if id "${name}" &>/dev/null; then
                MANIFEST[USER]="${name}"
            fi

            detail "Discovered project: ${name}"
            return 0
        fi
    done

    return 1
}

# ------------------------------------------------------------------------------
# Removal Tracking
# ------------------------------------------------------------------------------
# What: Track what was removed, kept, or failed
# Why: Summary at end shows user what happened
# Edge cases: Items might not exist (that's fine, skip silently)
# User impact: Clear record of actions taken
#

declare -a REMOVED=()
declare -a KEPT=()
declare -a ERRORS=()

remove_item() {
    local item="$1"
    local description="$2"

    # Skip if doesn't exist
    if [[ ! -e "${item}" ]]; then
        return 0
    fi

    if [[ "${DRY_RUN}" == "true" ]]; then
        info "[DRY-RUN] Would remove: ${item}"
        return 0
    fi

    if rm -rf "${item}"; then
        REMOVED+=("${description}: ${item}")
        detail "Removed: ${item}"
    else
        ERRORS+=("Failed to remove: ${item}")
        warn "Failed to remove: ${item}"
    fi
}

keep_item() {
    local item="$1"
    local description="$2"

    if [[ -e "${item}" ]]; then
        KEPT+=("${description}: ${item}")
    fi
}

# ------------------------------------------------------------------------------
# Service Management
# ------------------------------------------------------------------------------
# What: Stop and disable the systemd service
# Why: Can't remove files if service is running
# Edge cases: Service might not be running/enabled (that's fine)
# User impact: Service is cleanly stopped
#

stop_service() {
    local service="${MANIFEST[PROJECT_NAME]:-}"
    [[ -z "${service}" ]] && return 0

    if [[ "${DRY_RUN}" == "true" ]]; then
        if systemctl is-active --quiet "${service}" 2>/dev/null; then
            info "[DRY-RUN] Would stop service: ${service}"
        fi
        if systemctl is-enabled --quiet "${service}" 2>/dev/null; then
            info "[DRY-RUN] Would disable service: ${service}"
        fi
        return 0
    fi

    if systemctl is-active --quiet "${service}" 2>/dev/null; then
        action "Stopping service: ${service}"
        systemctl stop "${service}" 2>/dev/null || true
    fi

    if systemctl is-enabled --quiet "${service}" 2>/dev/null; then
        action "Disabling service: ${service}"
        systemctl disable "${service}" 2>/dev/null || true
    fi
}

# ------------------------------------------------------------------------------
# Removal Functions
# ------------------------------------------------------------------------------

remove_service_file() {
    local service_file="${MANIFEST[SERVICE_FILE]:-}"
    [[ -z "${service_file}" ]] && return 0

    remove_item "${service_file}" "Service file"

    if [[ "${DRY_RUN}" != "true" ]]; then
        systemctl daemon-reload 2>/dev/null || true
    fi
}

remove_binary() {
    local binary="${MANIFEST[BINARY]:-}"
    [[ -z "${binary}" ]] && return 0

    remove_item "${binary}" "Binary"
}

remove_config_dir() {
    local etc_dir="${MANIFEST[ETC_DIR]:-}"
    [[ -z "${etc_dir}" ]] && return 0

    if [[ "${PURGE}" == "true" ]]; then
        remove_item "${etc_dir}" "Config directory"
    else
        keep_item "${etc_dir}" "Config directory (use --purge to remove)"
    fi
}

remove_data_dir() {
    local var_dir="${MANIFEST[VAR_DIR]:-}"
    [[ -z "${var_dir}" ]] && return 0

    if [[ "${PURGE}" == "true" ]]; then
        remove_item "${var_dir}" "Data directory"
    else
        keep_item "${var_dir}" "Data directory (use --purge to remove)"
    fi
}

remove_log_dir() {
    local log_dir="${MANIFEST[LOG_DIR]:-}"
    [[ -z "${log_dir}" ]] && return 0

    if [[ "${PURGE}" == "true" ]]; then
        remove_item "${log_dir}" "Log directory"
    else
        keep_item "${log_dir}" "Log directory (use --purge to remove)"
    fi
}

remove_user() {
    local user="${MANIFEST[USER]:-}"
    [[ -z "${user}" ]] && return 0

    # Only remove user in purge mode
    if [[ "${PURGE}" != "true" ]]; then
        if id "${user}" &>/dev/null; then
            keep_item "${user}" "Service user (use --purge to remove)"
        fi
        return 0
    fi

    if ! id "${user}" &>/dev/null; then
        return 0
    fi

    if [[ "${DRY_RUN}" == "true" ]]; then
        info "[DRY-RUN] Would remove user: ${user}"
        return 0
    fi

    action "Removing user: ${user}"
    if userdel "${user}" 2>/dev/null; then
        REMOVED+=("User: ${user}")
    else
        ERRORS+=("Failed to remove user: ${user}")
    fi
}

# ------------------------------------------------------------------------------
# Summary
# ------------------------------------------------------------------------------

print_summary() {
    echo ""
    echo "========================================"
    echo "  Uninstall Summary"
    echo "========================================"

    if [[ ${#REMOVED[@]} -gt 0 ]]; then
        echo -e "${GREEN}Removed:${NC}"
        for item in "${REMOVED[@]}"; do
            echo "  - ${item}"
        done
    fi

    if [[ ${#KEPT[@]} -gt 0 ]]; then
        echo -e "${YELLOW}Kept (preserved):${NC}"
        for item in "${KEPT[@]}"; do
            echo "  - ${item}"
        done
    fi

    if [[ ${#ERRORS[@]} -gt 0 ]]; then
        echo -e "${RED}Errors:${NC}"
        for item in "${ERRORS[@]}"; do
            echo "  - ${item}"
        done
    fi

    echo ""

    if [[ "${DRY_RUN}" == "true" ]]; then
        warn "DRY RUN - no changes were made"
        echo ""
        echo "To actually uninstall, run without --dry-run"
        return 0
    fi

    if [[ ${#ERRORS[@]} -eq 0 ]]; then
        success "Uninstall complete"
        if [[ ${#KEPT[@]} -gt 0 ]]; then
            echo ""
            echo "To remove preserved items: sudo $0 --purge"
        fi
    else
        error "Uninstall completed with errors"
        return 1
    fi
}

# ------------------------------------------------------------------------------
# Purge Confirmation
# ------------------------------------------------------------------------------
# What: Require explicit confirmation for destructive purge
# Why: Purge removes all data - user should be sure
# Edge cases: Non-interactive environments can't confirm (use with caution)
# User impact: Safety check before data loss
#

confirm_purge() {
    if [[ "${PURGE}" != "true" ]]; then
        return 0
    fi

    if [[ "${DRY_RUN}" == "true" ]]; then
        # Dry run doesn't need confirmation
        return 0
    fi

    echo ""
    warn "PURGE MODE: This will permanently remove:"
    echo "  - Configuration files in ${MANIFEST[ETC_DIR]:-/etc/<project>}"
    echo "  - Data files in ${MANIFEST[VAR_DIR]:-/var/lib/<project>}"
    echo "  - Log files in ${MANIFEST[LOG_DIR]:-/var/log/<project>}"
    echo "  - Service user ${MANIFEST[USER]:-<project>}"
    echo ""

    # Check if we're in a terminal
    if [[ -t 0 ]]; then
        read -rp "Type 'yes' to confirm purge: " confirm
        if [[ "${confirm}" != "yes" ]]; then
            echo "Aborted."
            exit 1
        fi
    else
        # Non-interactive - require explicit confirmation via env var
        if [[ "${UNINSTALL_CONFIRM_PURGE:-}" != "yes" ]]; then
            error "Non-interactive purge requires UNINSTALL_CONFIRM_PURGE=yes"
            exit 1
        fi
    fi

    echo ""
}

# ------------------------------------------------------------------------------
# Main
# ------------------------------------------------------------------------------

main() {
    echo "========================================"
    echo "  Water-Treat Uninstaller"
    echo "========================================"
    echo ""

    parse_args "$@"

    # Must be root
    check_root || exit 1

    if [[ "${DRY_RUN}" == "true" ]]; then
        info "DRY RUN MODE - no changes will be made"
        echo ""
    fi

    if [[ "${PURGE}" == "true" ]]; then
        warn "PURGE MODE - will remove all data"
        echo ""
    fi

    # Find and read manifest
    header "Discovery"

    local manifest_file
    if manifest_file="$(find_manifest)"; then
        read_manifest "${manifest_file}"
    elif discover_from_system; then
        detail "Discovered installation from system state"
    else
        warn "No installation found"
        echo ""
        echo "No Water-Treat installation was detected."
        echo "If you believe this is wrong, check:"
        echo "  - /etc/systemd/system/*.service"
        echo "  - /usr/local/bin/"
        exit 0
    fi

    # Confirm purge if requested
    confirm_purge

    # Removal
    header "Removal"
    stop_service
    remove_service_file
    remove_binary
    remove_config_dir
    remove_data_dir
    remove_log_dir
    remove_user

    # Summary
    print_summary
}

main "$@"

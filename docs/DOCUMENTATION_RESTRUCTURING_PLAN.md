# Documentation Restructuring Plan

**Generated:** 2025-12-22
**Target:** Hybrid documentation model for Water-Treat and Water-Controller

---

## Target Structure

```
docs/
├── generated/                          # Git-ignored, rebuilt by CI
│   ├── api/                            # Doxygen API reference
│   ├── coverage/                       # Test coverage reports
│   ├── diagrams/                       # Generated architecture diagrams
│   └── schemas/                        # Database schema documentation
├── versioned/
│   └── v1.0/                           # Frozen at each release
│       ├── operations-manual.md        # Operator procedures
│       ├── SAFETY_INTERLOCKS.md        # Safety-critical documentation
│       ├── ALARM_RESPONSE.md           # Alarm handling procedures
│       ├── COMMISSIONING.md            # System handoff checklist
│       └── data-format-spec.md         # PROFINET wire protocol
├── architecture/
│   ├── system-overview.md              # High-level architecture
│   ├── profinet-integration.md         # PROFINET details
│   └── data-flow.md                    # Data flow diagrams
├── development/
│   ├── setup.md                        # Developer environment setup
│   ├── testing.md                      # Test development guide
│   ├── drivers.md                      # Sensor driver development
│   └── contributing.md                 # Contribution guidelines
├── integration/                        # Cross-repository docs
│   ├── CONTROLLER_SPEC.md
│   ├── CONTROLLER_INTEGRATION_NOTES.md
│   └── CROSS_REFERENCE_MATRIX.md
└── CHANGELOG.md                        # Auto-generated from commits
```

---

## Migration Tasks

Organized by dependency chain. Complete prerequisites before dependents.

### Foundation (No Dependencies)

These can be completed in any order:

| Task | Priority | Effort | Description |
|------|----------|--------|-------------|
| Create `docs/generated/.gitignore` | High | Small | Add gitignore for generated docs directory |
| Create `docs/versioned/` structure | High | Small | Create versioned directory structure |
| Create `docs/architecture/` directory | Medium | Small | Create architecture docs directory |
| Create `docs/development/` directory | Medium | Small | Create development docs directory |
| Create CONTRIBUTING.md | Medium | Small | Basic contribution guidelines |
| Create .github/workflows/docs.yml | High | Medium | CI workflow for documentation generation |

**Implementation:**

```bash
# Create directory structure
mkdir -p docs/generated/{api,coverage,diagrams,schemas}
mkdir -p docs/versioned/v1.0
mkdir -p docs/architecture
mkdir -p docs/development
mkdir -p docs/integration

# Git-ignore generated directory
echo "*" > docs/generated/.gitignore
echo "!.gitignore" >> docs/generated/.gitignore
```

---

### Requires Foundation Complete

| Task | Priority | Effort | Dependencies | Description |
|------|----------|--------|--------------|-------------|
| Add Doxygen configuration | Medium | Small | Foundation | Create Doxyfile for API generation |
| Install Doxygen in CI | Medium | Small | .github/workflows/docs.yml | Add doxygen to CI workflow |
| Create SAFETY_INTERLOCKS.md | **Critical** | Medium | versioned/ structure | Document all safety-critical behaviors |
| Create ALARM_RESPONSE.md | **Critical** | Medium | versioned/ structure | Document alarm response procedures |
| Create COMMISSIONING.md | High | Medium | versioned/ structure | System handoff checklist |
| Move existing integration docs | High | Small | integration/ structure | Reorganize existing docs |
| Create system-overview.md | Medium | Medium | architecture/ structure | High-level architecture document |

**Doxygen Configuration (Doxyfile):**

```
PROJECT_NAME           = "Water-Treat RTU"
PROJECT_NUMBER         = 1.0
OUTPUT_DIRECTORY       = docs/generated/api
INPUT                  = src
RECURSIVE              = YES
GENERATE_HTML          = YES
GENERATE_LATEX         = NO
EXTRACT_ALL            = YES
EXTRACT_PRIVATE        = NO
EXTRACT_STATIC         = NO
OPTIMIZE_OUTPUT_FOR_C  = YES
FILE_PATTERNS          = *.c *.h
EXCLUDE_PATTERNS       = */test/*
```

---

### Requires Prior Tiers Complete

| Task | Priority | Effort | Dependencies | Description |
|------|----------|--------|--------------|-------------|
| Add doc comments to public API | Medium | Large | Doxygen config | Add /** */ comments to all public functions |
| Create data-flow.md with diagrams | Medium | Medium | system-overview.md | Data flow documentation |
| Create testing.md | Medium | Small | development/ structure | Test development guide |
| Create drivers.md | Medium | Medium | development/ structure | Sensor driver development guide |
| Setup changelog automation | High | Small | CI workflow | Auto-generate CHANGELOG.md |
| Create v1.0 operations-manual.md | High | Small | SAFETY_INTERLOCKS.md | Snapshot of OPERATOR.md for release |
| Create v1.0 data-format-spec.md | High | Small | versioned/ structure | Snapshot of PROFINET spec |

---

### Ongoing (Continuous Process)

| Task | Priority | Frequency | Description |
|------|----------|-----------|-------------|
| Update CHANGELOG.md | High | Each release | Maintain version history |
| Version freeze at release | High | Each release | Copy current docs to versioned/vX.Y/ |
| Review doc accuracy | Medium | Monthly | Compare docs to code changes |
| Update API doc comments | Medium | With code changes | Keep doc comments current |
| Update OPERATOR.md | High | Feature changes | Keep operator manual current |

---

## Detailed Task Specifications

### Task: Create SAFETY_INTERLOCKS.md

**Priority:** Critical
**Effort:** Medium
**Location:** `docs/versioned/v1.0/SAFETY_INTERLOCKS.md`

**Required Content:**
1. All actuator safe states and their conditions
2. PROFINET connection loss behavior
3. Sensor fault handling
4. Emergency shutdown procedures
5. Recovery procedures after fault

**Source Information:**
- `src/actuators/actuator_manager.c` - safe state implementation
- `src/profinet/profinet_device.c` - connection loss handling
- `OPERATOR.md` - partial safe state documentation
- `CONTROLLER_INTEGRATION_NOTES.md` - safe state behavior notes

**Template provided below.**

---

### Task: Create ALARM_RESPONSE.md

**Priority:** Critical
**Effort:** Medium
**Location:** `docs/versioned/v1.0/ALARM_RESPONSE.md`

**Required Content:**
1. All alarm conditions and thresholds
2. Severity classifications (ISA-18.2 alignment)
3. Operator response for each alarm
4. Escalation procedures
5. Alarm acknowledgment process

**Source Information:**
- `src/alarms/alarm_manager.c` - alarm implementation
- `CONTROLLER_SPEC.md` - ISA-18.2 alarm states
- Sensor threshold defaults in database schema

**Template provided below.**

---

### Task: Create COMMISSIONING.md

**Priority:** High
**Effort:** Medium
**Location:** `docs/versioned/v1.0/COMMISSIONING.md`

**Required Content:**
1. Pre-commissioning checklist (hardware verification)
2. Software installation verification
3. Sensor calibration procedures
4. Actuator function testing
5. PROFINET communication verification
6. Alarm testing
7. Safe state verification
8. Documentation handoff

**Source Information:**
- `INSTALL.md` - installation procedures
- `OPERATOR.md` - wiring and configuration
- `gsd/GSDML-V2.4-*.xml` - PROFINET configuration

**Template provided below.**

---

### Task: Setup Changelog Automation

**Priority:** High
**Effort:** Small

**Implementation Options:**

1. **Conventional Commits + changelog-generator**
   - Enforce commit message format
   - Auto-generate changelog on release

2. **GitHub Releases**
   - Use GitHub release notes
   - Copy to CHANGELOG.md on release

**Recommended Commit Format:**
```
<type>(<scope>): <description>

Types: feat, fix, docs, refactor, test, chore
Scope: profinet, sensors, actuators, tui, alarms, etc.

Examples:
feat(sensors): add ADS1115 ADC driver
fix(profinet): correct byte order for sensor data
docs(operator): update wiring diagram for pH probe
```

---

## GitHub Actions Workflow

**File:** `.github/workflows/docs.yml`

```yaml
name: Documentation

on:
  push:
    branches: [main]
    paths:
      - 'src/**'
      - 'docs/**'
      - 'Doxyfile'
  release:
    types: [published]
  workflow_dispatch:

jobs:
  generate-docs:
    runs-on: ubuntu-latest

    steps:
      - name: Checkout repository
        uses: actions/checkout@v4
        with:
          fetch-depth: 0  # Full history for changelog

      - name: Install dependencies
        run: |
          sudo apt-get update
          sudo apt-get install -y doxygen graphviz lcov

      - name: Generate API documentation
        run: |
          mkdir -p docs/generated/api
          doxygen Doxyfile

      - name: Generate dependency diagrams
        run: |
          mkdir -p docs/generated/diagrams
          # Generate include dependency graph
          doxygen -g - | \
            sed 's/HAVE_DOT.*/HAVE_DOT = YES/' | \
            sed 's/CALL_GRAPH.*/CALL_GRAPH = YES/' | \
            sed 's/CALLER_GRAPH.*/CALLER_GRAPH = YES/' > Doxyfile.diagrams
          doxygen Doxyfile.diagrams

      - name: Build and test for coverage
        if: github.event_name != 'release'
        run: |
          mkdir -p build && cd build
          cmake -DCMAKE_BUILD_TYPE=Debug -DENABLE_COVERAGE=ON ..
          make -j$(nproc)
          ctest --output-on-failure
          lcov --capture --directory . --output-file coverage.info
          lcov --remove coverage.info '/usr/*' --output-file coverage.info
          genhtml coverage.info --output-directory ../docs/generated/coverage

      - name: Upload documentation artifacts
        uses: actions/upload-artifact@v4
        with:
          name: documentation
          path: docs/generated/
          retention-days: 30

      - name: Deploy to GitHub Pages
        if: github.ref == 'refs/heads/main'
        uses: peaceiris/actions-gh-pages@v3
        with:
          github_token: ${{ secrets.GITHUB_TOKEN }}
          publish_dir: ./docs/generated
          destination_dir: api

  version-docs:
    runs-on: ubuntu-latest
    if: github.event_name == 'release'

    steps:
      - name: Checkout repository
        uses: actions/checkout@v4

      - name: Get version from tag
        id: version
        run: echo "version=${GITHUB_REF#refs/tags/v}" >> $GITHUB_OUTPUT

      - name: Create versioned documentation
        run: |
          VERSION=${{ steps.version.outputs.version }}
          mkdir -p docs/versioned/v${VERSION}

          # Copy current operator documentation
          cp OPERATOR.md docs/versioned/v${VERSION}/operations-manual.md

          # Copy PROFINET specification
          cp docs/PROFINET_DATA_FORMAT_SPECIFICATION.md \
             docs/versioned/v${VERSION}/data-format-spec.md

          # Copy safety docs if they exist
          [ -f docs/SAFETY_INTERLOCKS.md ] && \
            cp docs/SAFETY_INTERLOCKS.md docs/versioned/v${VERSION}/
          [ -f docs/ALARM_RESPONSE.md ] && \
            cp docs/ALARM_RESPONSE.md docs/versioned/v${VERSION}/
          [ -f docs/COMMISSIONING.md ] && \
            cp docs/COMMISSIONING.md docs/versioned/v${VERSION}/

      - name: Generate changelog
        run: |
          # Get commits since last tag
          PREV_TAG=$(git describe --tags --abbrev=0 HEAD^ 2>/dev/null || echo "")
          if [ -n "$PREV_TAG" ]; then
            echo "## ${{ github.ref_name }} ($(date +%Y-%m-%d))" > CHANGELOG_NEW.md
            echo "" >> CHANGELOG_NEW.md
            git log ${PREV_TAG}..HEAD --pretty=format:"- %s" >> CHANGELOG_NEW.md
            echo "" >> CHANGELOG_NEW.md
            echo "" >> CHANGELOG_NEW.md
            cat CHANGELOG.md >> CHANGELOG_NEW.md 2>/dev/null || true
            mv CHANGELOG_NEW.md CHANGELOG.md
          fi

      - name: Commit versioned docs
        run: |
          git config user.name "github-actions[bot]"
          git config user.email "github-actions[bot]@users.noreply.github.com"
          git add docs/versioned/ CHANGELOG.md
          git commit -m "docs: add versioned documentation for ${{ github.ref_name }}" || true
          git push
```

---

## Critical Documentation Templates

### Template: SAFETY_INTERLOCKS.md

```markdown
# Safety Interlocks Documentation

**Version:** 1.0
**Last Updated:** [DATE]
**Document Owner:** [NAME]

## 1. Overview

This document describes all safety-critical behaviors implemented in the
Water Treatment RTU firmware. It is a controlled document and must be
updated whenever safety-related functionality changes.

## 2. Definitions

| Term | Definition |
|------|------------|
| Safe State | The state an actuator assumes when a fault condition occurs |
| Interlock | A condition that prevents or forces an actuator state |
| Fault | Any condition that triggers safe state behavior |

## 3. Actuator Safe States

| Actuator Type | Default Safe State | Configurable | Rationale |
|---------------|-------------------|--------------|-----------|
| Pump | OFF (de-energize) | Yes | Prevent dry running, overflow |
| Valve | CLOSED (de-energize) | Yes | Prevent uncontrolled flow |
| Generic Relay | OFF (de-energize) | Yes | Fail-safe default |

### 3.1 Safe State Triggers

Safe state is activated when ANY of the following conditions occur:

1. **PROFINET Connection Loss**
   - Trigger: No valid PROFINET frame received for [configurable] ms
   - Default timeout: 500 ms
   - Action: All actuators go to configured safe state
   - Recovery: Automatic when connection restored

2. **Controller Watchdog Timeout**
   - Trigger: Controller stops sending valid commands
   - Timeout: Configured per-actuator
   - Action: Individual actuator goes to safe state

3. **Sensor Fault on Interlock Input**
   - Trigger: IOPS status = BAD (0x00) on interlock sensor
   - Action: Related actuators go to safe state
   - Recovery: Manual acknowledgment required

## 4. Implemented Interlocks

### 4.1 [TEMPLATE: Add each interlock]

**Interlock Name:** [e.g., Low Level Pump Cutoff]

| Property | Value |
|----------|-------|
| Condition Sensor | [sensor name, slot] |
| Threshold | [value with units] |
| Comparison | [BELOW/ABOVE/EQUAL] |
| Protected Actuator | [actuator name, slot] |
| Action | [FORCE_OFF/FORCE_ON/ALARM_ONLY] |
| Bypass Capable | [Yes/No] |
| Reset Mode | [AUTO/MANUAL] |

**Sequence of Events:**
1. [Describe what happens when interlock trips]
2. [Describe operator notification]
3. [Describe recovery procedure]

## 5. Emergency Shutdown

### 5.1 Manual Emergency Stop

[Document E-STOP behavior if implemented]

### 5.2 Automatic Shutdown Conditions

[Document conditions that trigger full system shutdown]

## 6. Recovery Procedures

### 6.1 After PROFINET Connection Loss

1. Verify network connectivity
2. Check controller status
3. Actuators will resume normal operation automatically when connection restored
4. Verify process state before resuming control

### 6.2 After Sensor Fault

1. Investigate sensor fault cause
2. Repair or replace sensor
3. Verify sensor reading is valid (IOPS = GOOD)
4. Acknowledge fault in TUI or via controller
5. Actuator will return to controller command

## 7. Testing Requirements

All safety interlocks MUST be tested:
- During commissioning
- After any firmware update
- After any configuration change affecting interlocks
- Periodically as defined by site procedures

## 8. Change History

| Version | Date | Author | Changes |
|---------|------|--------|---------|
| 1.0 | [DATE] | [NAME] | Initial release |
```

---

### Template: ALARM_RESPONSE.md

```markdown
# Alarm Response Procedures

**Version:** 1.0
**Last Updated:** [DATE]
**Document Owner:** [NAME]

## 1. Alarm System Overview

The Water Treatment RTU generates alarms when process conditions exceed
configured thresholds. Alarms are transmitted to the PROFINET controller
and displayed on the local TUI.

## 2. Alarm Severity Levels

| Severity | Response Time | Description |
|----------|---------------|-------------|
| EMERGENCY | Immediate | Automatic action required, potential safety hazard |
| HIGH | < 5 minutes | Abnormal condition, operator action required |
| MEDIUM | < 1 hour | Abnormal condition, action required within shift |
| LOW | Informational | Status change, no immediate action required |

## 3. Alarm Conditions

### 3.1 pH Alarms

| Alarm | Threshold | Severity | Cause | Response |
|-------|-----------|----------|-------|----------|
| pH High-High | > 9.0 | HIGH | Over-dosing base, sensor fault | 1. Check dosing pump, 2. Verify sensor |
| pH High | > 8.5 | MEDIUM | Base dosing imbalance | Adjust setpoint or verify sensor |
| pH Low | < 6.5 | MEDIUM | Acid dosing imbalance | Adjust setpoint or verify sensor |
| pH Low-Low | < 6.0 | HIGH | Over-dosing acid, sensor fault | 1. Check dosing pump, 2. Verify sensor |
| pH Sensor Fault | IOPS=BAD | HIGH | Sensor failure | Replace or recalibrate sensor |

### 3.2 Level Alarms

| Alarm | Threshold | Severity | Cause | Response |
|-------|-----------|----------|-------|----------|
| Level High-High | > 95% | EMERGENCY | Overflow imminent | 1. Stop inflow, 2. Start drain |
| Level High | > 90% | HIGH | Tank filling | Reduce inflow or increase outflow |
| Level Low | < 15% | MEDIUM | Tank draining | Increase inflow or reduce outflow |
| Level Low-Low | < 10% | HIGH | Pump cavitation risk | 1. Stop pumps, 2. Increase inflow |

### 3.3 Flow Alarms

| Alarm | Threshold | Severity | Cause | Response |
|-------|-----------|----------|-------|----------|
| Flow High | > [setpoint] | MEDIUM | Valve issue, demand spike | Verify valve position |
| Flow Low | < [setpoint] | MEDIUM | Blockage, pump issue | Check for obstructions |
| No Flow | = 0 when expected | HIGH | Pump failure, blockage | Check pump operation |

### 3.4 Communication Alarms

| Alarm | Condition | Severity | Cause | Response |
|-------|-----------|----------|-------|----------|
| PROFINET Disconnect | No connection | HIGH | Network issue | Check cables, controller |
| Controller Timeout | No commands | HIGH | Controller issue | Check controller status |
| Sensor Timeout | No reading | MEDIUM | Sensor/wiring issue | Check sensor wiring |

## 4. Alarm Acknowledgment

### 4.1 Via Local TUI
1. Navigate to Alarms screen (F6)
2. Select alarm with arrow keys
3. Press 'A' to acknowledge
4. Alarm moves to ACKNOWLEDGED state

### 4.2 Via Controller/HMI
[Document controller-side acknowledgment if supported]

## 5. Alarm Shelving

Alarm shelving allows temporary suppression of nuisance alarms during
known conditions (e.g., maintenance).

**Procedure:**
1. [Document shelving procedure]
2. Maximum shelve duration: [time]
3. Shelved alarms are logged with operator ID

## 6. Escalation Procedures

| Condition | Escalation Action |
|-----------|------------------|
| EMERGENCY alarm unacknowledged > 5 min | Notify supervisor |
| HIGH alarm unacknowledged > 15 min | Notify supervisor |
| Multiple simultaneous HIGH alarms | Notify supervisor immediately |
| Recurring alarm (> 3x in 1 hour) | Investigate root cause |

## 7. Alarm Testing

Alarms must be tested:
- During commissioning
- After threshold changes
- Periodically per site procedures

**Test Procedure:**
1. Note current process values
2. Temporarily adjust threshold to trigger alarm
3. Verify alarm appears on TUI and controller
4. Verify correct severity and message
5. Acknowledge alarm
6. Restore original threshold
7. Verify alarm clears

## 8. Change History

| Version | Date | Author | Changes |
|---------|------|--------|---------|
| 1.0 | [DATE] | [NAME] | Initial release |
```

---

### Template: COMMISSIONING.md

```markdown
# Commissioning Checklist

**Version:** 1.0
**Project:** [Project Name]
**RTU Station Name:** [e.g., rtu-tank-1]
**Date:** [DATE]
**Commissioning Engineer:** [NAME]

---

## 1. Pre-Commissioning Verification

### 1.1 Hardware Checklist

| Item | Verified | Notes |
|------|----------|-------|
| SBC physically mounted | [ ] | |
| Power supply connected (5V DC) | [ ] | |
| Ethernet cable connected | [ ] | |
| All sensor wiring complete | [ ] | |
| All actuator wiring complete | [ ] | |
| Proper wire terminations | [ ] | |
| Cable strain relief installed | [ ] | |
| Enclosure sealed (if applicable) | [ ] | |

### 1.2 Wiring Verification

Verify against wiring diagram in OPERATOR.md:

| Connection | Terminal | Wire Color | Verified |
|------------|----------|------------|----------|
| [Sensor 1] | [GPIO/I2C] | [Color] | [ ] |
| [Sensor 2] | [GPIO/I2C] | [Color] | [ ] |
| [Actuator 1] | [GPIO] | [Color] | [ ] |

---

## 2. Software Installation Verification

### 2.1 OS and Firmware

| Item | Expected | Actual | Pass |
|------|----------|--------|------|
| Operating System | [e.g., Raspberry Pi OS] | | [ ] |
| Kernel Version | 6.1+ | | [ ] |
| p-net Library | Installed | | [ ] |
| Water-Treat Firmware | v[X.Y.Z] | | [ ] |

**Verification Commands:**
```bash
uname -r                    # Kernel version
dpkg -l | grep pnet        # p-net installed
profinet-monitor --version  # Firmware version
```

### 2.2 Service Status

| Service | Status | Pass |
|---------|--------|------|
| profinet-monitor.service | active (running) | [ ] |

**Verification:**
```bash
sudo systemctl status profinet-monitor
```

---

## 3. Network Configuration

### 3.1 Network Settings

| Parameter | Expected | Actual | Pass |
|-----------|----------|--------|------|
| IP Address | [X.X.X.X] | | [ ] |
| Subnet Mask | [X.X.X.X] | | [ ] |
| Gateway | [X.X.X.X] | | [ ] |
| PROFINET Station Name | [name] | | [ ] |

### 3.2 Controller Connectivity

| Test | Result | Pass |
|------|--------|------|
| Ping controller | | [ ] |
| PROFINET discovery visible | | [ ] |
| AR established | | [ ] |

---

## 4. Sensor Calibration

### 4.1 [Sensor Name]

| Parameter | Expected | Measured | Deviation | Pass |
|-----------|----------|----------|-----------|------|
| Zero point | | | | [ ] |
| Span check | | | | [ ] |
| Reading at known value | | | | [ ] |

### 4.2 [Repeat for each sensor]

---

## 5. Actuator Function Testing

### 5.1 [Actuator Name]

| Test | Expected | Observed | Pass |
|------|----------|----------|------|
| OFF command | Actuator de-energized | | [ ] |
| ON command | Actuator energized | | [ ] |
| PWM 50% (if applicable) | 50% duty cycle | | [ ] |
| Safe state test | Returns to safe state | | [ ] |

### 5.2 [Repeat for each actuator]

---

## 6. PROFINET Communication Verification

### 6.1 Cyclic Data Exchange

| Test | Expected | Observed | Pass |
|------|----------|----------|------|
| Sensor values appear in controller | Yes | | [ ] |
| Actuator commands reach RTU | Yes | | [ ] |
| Cycle time | [X] ms | | [ ] |
| Data quality indicators | GOOD (0x80) | | [ ] |

### 6.2 Alarm Transmission

| Alarm Type | Triggered | Received at Controller | Pass |
|------------|-----------|----------------------|------|
| High alarm | [ ] | [ ] | [ ] |
| Low alarm | [ ] | [ ] | [ ] |
| Sensor fault | [ ] | [ ] | [ ] |

---

## 7. Safe State Testing

**WARNING: Ensure process is in safe condition before testing.**

### 7.1 PROFINET Connection Loss

| Step | Expected | Observed | Pass |
|------|----------|----------|------|
| 1. Disconnect Ethernet | | | |
| 2. Wait for timeout | Actuators go to safe state | | [ ] |
| 3. Verify actuator states | All OFF (or configured safe state) | | [ ] |
| 4. Reconnect Ethernet | Communication restored | | [ ] |
| 5. Resume normal operation | Actuators respond to commands | | [ ] |

### 7.2 Interlock Testing

| Interlock | Trigger Condition | Expected Action | Observed | Pass |
|-----------|-------------------|-----------------|----------|------|
| [Name] | [Condition] | [Action] | | [ ] |

---

## 8. Alarm Testing

| Alarm | Trigger Method | Alarm Raised | Acknowledged | Cleared | Pass |
|-------|---------------|--------------|--------------|---------|------|
| [Alarm 1] | | [ ] | [ ] | [ ] | [ ] |
| [Alarm 2] | | [ ] | [ ] | [ ] | [ ] |

---

## 9. Documentation Handoff

| Document | Provided | Location |
|----------|----------|----------|
| As-built wiring diagram | [ ] | |
| Sensor calibration records | [ ] | |
| Configuration backup | [ ] | |
| Operator training completed | [ ] | |

---

## 10. Sign-Off

| Role | Name | Signature | Date |
|------|------|-----------|------|
| Commissioning Engineer | | | |
| Site Operator | | | |
| Project Manager | | | |

---

## 11. Issues and Punch List

| Issue | Severity | Resolution | Status |
|-------|----------|------------|--------|
| | | | |

---

## 12. Notes

[Additional notes, observations, or recommendations]
```

---

## Implementation Checklist Summary

### Immediate Actions (This Sprint)

- [ ] Create `docs/generated/.gitignore`
- [ ] Create versioned directory structure
- [ ] Create `.github/workflows/docs.yml`
- [ ] Create SAFETY_INTERLOCKS.md template with actual values
- [ ] Create ALARM_RESPONSE.md template with actual values
- [ ] Create COMMISSIONING.md checklist

### Near-Term Actions (Next Sprint)

- [ ] Add Doxyfile configuration
- [ ] Add doc comments to critical public functions
- [ ] Create system-overview.md architecture document
- [ ] Setup CHANGELOG.md automation
- [ ] Move integration docs to docs/integration/

### Ongoing Actions

- [ ] Maintain doc comments with code changes
- [ ] Update CHANGELOG.md with releases
- [ ] Create versioned snapshots at each release
- [ ] Review documentation accuracy quarterly

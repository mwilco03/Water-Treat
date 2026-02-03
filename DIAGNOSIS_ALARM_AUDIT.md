# PROFINET Diagnosis Alarm Implementation Audit

**Date**: 2026-02-03
**Session**: https://claude.ai/code/session_0174Z21nxGp4fSJVUxiJJkhx

## Executive Summary

**CRITICAL BUG DISCOVERED**: The current implementation of `profinet_manager_send_diagnosis()` uses the **wrong p-net API** for sending channel diagnosis alarms. This causes:

1. ❌ Alarm type (appears 0x0001 / disappears 0x0002) is **calculated but never sent**
2. ❌ Using `pnet_alarm_send_process_alarm()` instead of diagnosis-specific API
3. ❌ May not trigger standard PROFINET diagnosis alarm behavior on the controller
4. ❌ Diagnosis state is not stored in the IO-Device as required by PROFINET spec

## Timeline of Events

### 2026-02-03 18:39 - Initial "Fix" (WRONG)
**Commit**: `306f69c` - "fix: remove unused alarm_type variable"

I removed this line from `profinet_manager_send_diagnosis()`:
```c
uint16_t alarm_type = is_fault ? 0x0001 : 0x0002;  /* Appears / Disappears */
```

**Problem**: I fixed the compiler warning but didn't investigate WHY the variable existed.

### 2026-02-03 15:27 - Original Implementation
**Commit**: `88191bcf` - "fix: PROFINET resilience"

The code was added with this comment:
```c
/*
 * alarm_type values used:
 *   0x0001 = Diagnosis (channel fault appears)
 *   0x0002 = Diagnosis disappears (channel fault cleared)
 */
```

But then `alarm_type` was calculated and **never used**.

## Root Cause Analysis

### The Fundamental Mistake

The implementation uses `pnet_alarm_send_process_alarm()` which is for **process alarms** (e.g., "temperature too high"), not **diagnosis alarms** (e.g., "sensor disconnected").

### Process Alarms vs Diagnosis Alarms

| Aspect | Process Alarms | Diagnosis Alarms |
|--------|----------------|------------------|
| **Purpose** | Monitor external process conditions | Report device hardware faults |
| **API** | `pnet_alarm_send_process_alarm()` | `pnet_diag_add()` / `pnet_diag_remove()` |
| **Storage** | Not persisted in device | **Stored in IO-Device** per spec |
| **Automatic alarm** | Manual - app triggers | **Automatic** - stack sends appears/disappears |
| **Alarm type** | Application decides | **Auto: 0x0001 (add) / 0x0002 (remove)** |
| **Example** | Temperature exceeds setpoint | Wire break, sensor not available |

### What Should Happen

According to [p-net API documentation](https://github.com/rtlabs-com/p-net/blob/master/include/pnet_api.h):

> **Diagnosis Alarms** are automatically triggered when you add, update, or remove diagnostic
> information using the `pnet_diag_*` functions. The stack manages their delivery to the controller.

**The alarm type is automatic!** When you call:
- `pnet_diag_add()` → Stack sends alarm type **0x0001** (diagnosis appears)
- `pnet_diag_remove()` → Stack sends alarm type **0x0002** (diagnosis disappears)

### What Actually Happens (Current Code)

```c
// WRONG: Using process alarm API for diagnosis
int ret = pnet_alarm_send_process_alarm(
    g_pn.pnet, g_pn.arep, 0,
    (uint16_t)slot, (uint16_t)subslot,
    0x8000,  /* USI: channel diagnosis */
    sizeof(diag_data), diag_data
);
```

**Problems**:
1. Alarm type (0x0001/0x0002) is never sent - calculated in `alarm_type` but not used
2. Process alarms may require ACK callback management (see `pnet_alarm_cnf()` requirement)
3. Diagnosis state not stored in device (violates PROFINET spec)
4. May confuse controller - process alarm with diagnosis USI (0x8000)

## Impact Assessment

### Functional Impact
- ⚠️ Controllers may not properly recognize diagnosis appears/disappears
- ⚠️ No persistent diagnosis storage in the RTU
- ⚠️ Alarm ACK management may be broken (process alarms require waiting for `pnet_alarm_cnf()`)

### Wire Protocol Impact
- ✅ Likely works at wire level (alarm payload is correct)
- ⚠️ But semantically wrong (process alarm vs diagnosis alarm)

### Cross-System Impact
From `sensor_manager.c:284`:
```c
if (is_fault != was_fault) {
    profinet_manager_send_diagnosis(upd->slot, 0, upd->quality);
}
```

Every sensor quality transition (GOOD↔BAD, GOOD↔NOT_CONNECTED) calls this broken function.

## Correct Implementation

### Current Code (WRONG)
```c
result_t profinet_manager_send_diagnosis(int slot, int subslot, data_quality_t quality) {
    // ... build diag_data ...

    bool is_fault = (quality == QUALITY_BAD || quality == QUALITY_NOT_CONNECTED);
    uint16_t alarm_type = is_fault ? 0x0001 : 0x0002;  /* NEVER USED! */

    // WRONG API!
    int ret = pnet_alarm_send_process_alarm(
        g_pn.pnet, g_pn.arep, 0,
        (uint16_t)slot, (uint16_t)subslot,
        0x8000, sizeof(diag_data), diag_data
    );
}
```

### Proposed Fix
```c
result_t profinet_manager_send_diagnosis(int slot, int subslot, data_quality_t quality) {
    bool is_fault = (quality == QUALITY_BAD || quality == QUALITY_NOT_CONNECTED);

    pnet_diag_source_t diag_source = {
        .api = 0,
        .slot = (uint16_t)slot,
        .subslot = (uint16_t)subslot,
        .ch = 0,  /* 0 = whole submodule */
        .ch_grouping = PNET_DIAG_CH_INDIVIDUAL_CHANNEL,
        .ch_direction = PNET_DIAG_CH_PROP_DIR_INPUT
    };

    uint16_t ch_error_type;
    switch (quality) {
        case QUALITY_NOT_CONNECTED:
            ch_error_type = 0x001F;  /* Sensor not available */
            break;
        case QUALITY_BAD:
            ch_error_type = 0x0008;  /* Data transmission impossible */
            break;
        default:
            ch_error_type = 0x0000;  /* No error */
            break;
    }

    int ret;
    if (is_fault) {
        /* Add diagnosis - stack automatically sends 0x0001 (appears) */
        ret = pnet_diag_add(
            g_pn.pnet, &diag_source,
            PNET_DIAG_CH_PROP_TYPE_INPUT,      /* Channel type */
            PNET_DIAG_CH_PROP_MAINT_REQUIRED,  /* Severity */
            ch_error_type,                      /* Error type */
            0,                                  /* ext_ch_error_type */
            0,                                  /* ext_ch_add_value */
            0,                                  /* qual_ch_qualifier */
            0x8000,                             /* USI */
            0,                                  /* manuf_data_len */
            NULL                                /* p_manuf_data */
        );
    } else {
        /* Remove diagnosis - stack automatically sends 0x0002 (disappears) */
        ret = pnet_diag_remove(
            g_pn.pnet, &diag_source,
            ch_error_type,
            0,      /* ext_ch_error_type */
            0x8000  /* USI */
        );
    }

    LOG_INFO("Sent diagnosis %s for slot %d.%d (quality=%s)",
             is_fault ? "APPEARS" : "DISAPPEARS",
             slot, subslot, quality_to_string(quality));

    return (ret == 0) ? RESULT_OK : RESULT_ERROR;
}
```

### Key Changes
1. ✅ Use `pnet_diag_add()` when fault appears
2. ✅ Use `pnet_diag_remove()` when fault clears
3. ✅ Stack automatically sends alarm type 0x0001/0x0002
4. ✅ Diagnosis stored in device per PROFINET spec
5. ✅ No need for `pnet_alarm_cnf()` callback management

## Additional Issues Found

### 1. No Other `pnet_diag_*` Usage
```bash
$ grep -r "pnet_diag_" /home/user/Water-Treat/src
# NO RESULTS
```

This confirms `profinet_manager_send_diagnosis()` is the only place that should be using diagnosis APIs, and it's using the wrong one.

### 2. Documentation Mismatch
`docs/CONTROLLER_IMPLEMENTATION_GUIDE.md` states:

> | Type | USI | Meaning |
> |------|-----|---------|
> | 0x0001 | 0x8000 | Diagnosis appears (sensor fault) |
> | 0x0002 | 0x8000 | Diagnosis disappears (sensor recovered) |

But the code doesn't actually send these alarm types correctly.

## Recommendations

### Immediate Actions
1. ✅ **Revert my "fix" commit** `306f69c` - it was correct to keep `alarm_type` as a reminder
2. 🔧 **Replace `pnet_alarm_send_process_alarm()` with `pnet_diag_add()`/`pnet_diag_remove()`**
3. ✅ **Test with actual PROFINET controller** - verify appears/disappears alarms
4. 📝 **Update documentation** to explain diagnosis alarm implementation

### Testing Checklist
- [ ] Sensor fault (GOOD → BAD): verify alarm type 0x0001 appears on controller
- [ ] Sensor recovery (BAD → GOOD): verify alarm type 0x0002 disappears on controller
- [ ] Diagnosis persistence: verify RTU stores diagnosis state across reconnects
- [ ] No alarm backpressure: verify no `pnet_alarm_cnf()` callback issues

### Long-Term
- Add unit tests for diagnosis alarm transitions
- Document the difference between process and diagnosis alarms in `CLAUDE.md`
- Consider alarm rate limiting if sensors flap frequently

## Commit History

1. **306f69c** - ❌ "fix: remove unused alarm_type variable" (WRONG - reverted)
2. **e3781a6** - "Revert 'fix: remove unused alarm_type variable'"
3. **3d43a44** - ✅ "fix: use correct p-net diagnosis API for channel diagnosis alarms"
4. **c665664** - ✅ "fix: correct p-net diagnosis API enum constants" (compilation fix)

All changes pushed to branch `claude/libgpiod-v2-setup-GmByI`.

### Enum Constant Correction (c665664)

After implementing the correct diagnosis API, the code had wrong enum constant names that caused compilation failure:

**Error**: `'PNET_DIAG_CH_PROP_TYPE_INPUT' undeclared`

The `ch_bits` parameter expects channel data width values (UNSPECIFIED, 1_BIT, 8_BIT, etc.), not channel direction.

**Fixed**:
- ch_bits: `PNET_DIAG_CH_PROP_TYPE_UNSPECIFIED` (for whole submodule)
- severity: `PNET_DIAG_CH_PROP_MAINT_FAULT` (fault, not just maintenance)

## Conclusion

The original developer (in commit `88191bcf`) **knew** that alarm type 0x0001/0x0002 was needed (as evidenced by the comment and the calculated `alarm_type` variable), but either:

1. Didn't know about `pnet_diag_add()`/`pnet_diag_remove()` API
2. Assumed `pnet_alarm_send_process_alarm()` could handle diagnosis alarms
3. Left it as a TODO (incomplete implementation)

When I saw the unused variable, I should have:
- ✅ Read the git history ✓
- ✅ Checked the p-net API documentation ✓
- ✅ Searched for similar patterns ✓
- ❌ **Asked the user first** ✗

**Lesson learned**: Never remove code that looks intentional without understanding its purpose.

## Sources

- [p-net API Header](https://github.com/rtlabs-com/p-net/blob/master/include/pnet_api.h)
- [p-net GitHub Repository](https://github.com/rtlabs-com/p-net)
- [PROFINET Alarm Design Guide](https://profinetuniversity.com/profinet-development/designing-alarms-for-profinet-devices/)
- [PROFINET Diagnostics Suite](https://profinetuniversity.com/profinet-diagnostics/profinet-diagnostics-suite-part-3/)

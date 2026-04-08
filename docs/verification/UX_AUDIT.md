# TUI UX Audit -- Sensor/Actuator Dialogs

Audit of the ncurses TUI add/edit/delete flows for sensors and actuators in the
Water-treat RTU. Target user: a field service technician on a KVM, SSH, or
PuTTY console, possibly tired, possibly one-handed, possibly the only person
on site. Findings are based on direct reading of the dialog implementation
files; every finding cites `file:line`.

## Executive summary

- 8 critical UX defects (block field-tech workflow or silently corrupt state)
- 12 high defects (significant friction, silently wrong behavior)
- 9 medium defects (annoyances, discoverability)
- 6 low defects (polish)

## Findings (sorted by severity)

---

### [CRITICAL] Editing a GPIO sensor is impossible: self-conflict on save

- **Location**: `src/tui/dialogs/dialog_sensor.c:192-240`, specifically lines `222-228`
- **Problem**: `check_gpio_conflict()` is called from `dialog_sensor_edit()` (line `433`) with the current `sensor_id` as `exclude_sensor_id`, but the function ignores it: it calls `db_actuator_gpio_conflict_check(db, gpio_pin, chip, 0, &conflict)` with a hard-coded `0` (line `222`). The block at lines `224-228` literally acknowledges the bug ("`We can't easily check this without knowing the conflicting sensor ID`") and then does nothing. As a result, every edit of a `DHT22`, `Float Switch`, or any sensor with `interface = "gpio"` will hit the GPIO conflict check, find *its own row*, raise `RESULT_ALREADY_EXISTS`, and refuse to save. The error message is technically truthful ("GPIO 17 is already in use by sensor 'water_level_1'") -- it just happens to be the same sensor.
- **Scenario**: Tech adds a float switch on GPIO 17. A week later, they need to bump the poll rate from 1000 ms to 500 ms. They open Edit, change the poll rate, hit Save. The dialog throws "GPIO 17 is already in use by sensor 'water_level_1'". They have no way to save -- not by deleting the GPIO pin (it's required), not by retyping the same pin, not by tabbing past it. The only workaround is to delete the sensor entirely and re-add it.
- **Fix**: Pass `exclude_sensor_id` through to `db_actuator_gpio_conflict_check()` -- the function already has an `exclude_id` parameter (used by the actuator dialog at `dialog_actuator.c:130` for the same purpose). Either extend `db_actuator_gpio_conflict_check` to discriminate sensor-vs-actuator excludes, or add a parallel `db_sensor_gpio_conflict_check` and call the right one based on `conflict.conflict_type`. Until fixed, edits of GPIO sensors are blocked.

---

### [CRITICAL] Sensor delete leaks: actuator manager and on-disk sub-records not cleaned up

- **Location**: `src/tui/dialogs/dialog_sensor.c:499-517` (`dialog_sensor_delete`), `src/tui/pages/page_sensors.c:243-255` (`sensors_on_delete`)
- **Problem**: `dialog_sensor_delete` calls only `db_module_delete(db, sensor_id)` (line `513`). It does *not* call `sensor_manager_reload_sensors()`, even though `dialog_sensor_edit` and `dialog_sensor_add` both do (`dialog_sensor.c:335` and `:479`). The page-level callback `sensors_on_delete` calls `tui_notify_sensor_changed(slot)`, which does forward to `tui_reload_sensors` -> `sensor_manager_reload_sensors` (`tui_common.c:66-71, 73-76`), so by chance the sensor manager *is* reloaded -- but only because the page callback is doing the dialog's job. Anyone who calls `dialog_sensor_delete` without going through this exact page is going to leak a running sensor instance. More importantly, the database has CASCADE constraints (per `database.c`), so the sub-record (`physical_sensors`, `adc_sensors`, etc.) is removed by SQLite, but no caller checks the sub-record was actually deleted, and no driver-side resources (open i2c file descriptors, claimed gpiochip lines, 1-Wire poll handles) are explicitly released by the dialog. The reload at the page layer is doing all the work *if it works*; the dialog itself has no contract.
- **Scenario**: Tech deletes a DHT22. The sensor list refreshes and the row is gone. Tech adds a new DHT22 on the same GPIO pin. The new sensor either fails to claim the line because the old gpiod handle was never released, or it works but reads stale data because both instances briefly point at the same pin.
- **Fix**: Move the `sensor_manager_reload_sensors()` call into `dialog_sensor_delete` itself so the contract holds regardless of caller, identical to the add and edit paths. Verify in `sensor_manager_reload_sensors` that removed sensors actually release their hardware resources before being freed.

---

### [CRITICAL] Actuator add/edit/delete never reloads the actuator manager

- **Location**: `src/tui/pages/page_actuators.c:334-419`, `src/tui/dialogs/dialog_actuator.c` (no reload call anywhere)
- **Problem**: There is no `actuator_manager_reload()` call anywhere in the TUI tree -- I grepped the entire `src/tui` directory. The function exists (`src/actuators/actuator_manager.c:802`) and is called from `main.c:405` at startup, but never from the TUI. After Add (line `348`), Edit (line `389`), or Delete (line `409`), the page calls `load_actuators()` which only refreshes the *display list* from the DB. The running `g_actuator_mgr` keeps its old instance table, old GPIO line handles, old PWM channels, and continues to operate as if the change never happened.
- **Scenario**: Tech adds an actuator for a chemical dosing pump on GPIO 22. The list shows the new actuator. Tech presses SPACE to manually toggle it on -- nothing happens, because the actuator manager has no instance for this slot. Or worse, tech *deletes* an actuator wired to a relay that's currently energized. The list says "deleted", the GPIO line is never released, the relay stays energized indefinitely. The tech has no way to know.
- **Fix**: After every successful add/edit/delete in `page_actuators.c` (lines `351`, `391`, `411`), call `actuator_manager_reload(&g_actuator_mgr)`. Add an `actuator_manager_reload` call inside `dialog_actuator_show` itself (post-save) and inside the delete path so the contract is intrinsic and not page-dependent. Verify `actuator_manager_reload` releases hardware resources for removed actuators.

---

### [CRITICAL] Sensor types `web_poll`, `calculated`, `static` are silently dropped on save

- **Location**: `src/tui/dialogs/dialog_sensor.c:42` (the type list), `:264-293` (`save_sensor`), `:373-402` (`dialog_sensor_edit` load), `:446-475` (edit save)
- **Problem**: `sensor_types[]` advertises five types: `physical`, `adc`, `web_poll`, `calculated`, `static`. The user can pick any of them in the Type field via `dialog_select` (line `138`). But the save function only handles `physical` and `adc` -- there are no `else if` branches for the other three. Selecting any of `web_poll`, `calculated`, or `static` creates a `db_module` row with `module_type` set to the selected string, but *no sub-record at all*. The dialog returns success and the row appears in the sensor list, but the sensor will never report a value and the SCADA controller will see a valid module slot with no driver attached.
- **Scenario**: Tech needs to pull tank level from a vendor REST API. They open Add Sensor, scroll Type to `web_poll`, Save. Dialog says success. Sensor appears in list with `web_poll` as the type. They go home. The next morning the controller reports the slot as offline. There is no error, no log, nothing in the UI to explain why. The wizard `dialog_io_wizard.c` doesn't expose these types either, so the only path that could create them is broken.
- **Fix**: Two acceptable resolutions:
  1. Remove `web_poll`, `calculated`, and `static` from `sensor_types[]` (line `42`) until they are implemented end-to-end. Document them as "future" in the README, not in the dropdown. Hide what does not work.
  2. Implement the missing branches in `save_sensor` and `dialog_sensor_edit` save block. The data model exists in `db/database.c:27,32,36`, so this is wiring, not a redesign.

Option 1 is the safe immediate fix; option 2 is the longer-term completeness.

---

### [CRITICAL] Sensor save creates phantom rows when sub-record creation fails

- **Location**: `src/tui/dialogs/dialog_sensor.c:261-293` (`save_sensor`)
- **Problem**: `db_module_create()` at line `261` is checked: failure returns early. But the subsequent `db_physical_sensor_create()` (line `277`) and `db_adc_sensor_create()` (line `292`) return values are *ignored*. If the sub-record insert fails (DB locked, NOT NULL violation, disk full), the parent `modules` row remains in the database with no matching child row. The dialog returns `RESULT_OK` and the sensor appears in the list but is fundamentally broken: any read query that expects the sub-record will fail. Same gap on edit (`dialog_sensor.c:459, 474`).
- **Scenario**: Disk fills up while tech is configuring sensors. The first sensor saves OK. The second `db_physical_sensor_create` fails silently. The list now shows two sensors but only one works. Tech sees the second one with status `unknown` and doesn't know whether to wait, restart, or delete-and-retry.
- **Fix**: Check the return value of every `db_*_sensor_create` and `db_*_sensor_update` call. On failure, roll back: call `db_module_delete(db, *sensor_id)` to remove the orphan parent row, return an actionable error to the dialog ("Database write failed -- check disk space"), and keep the form data so the tech can retry.

---

### [CRITICAL] Default sensor slot 1 collides with reserved CPU temp sensor

- **Location**: `src/tui/dialogs/dialog_sensor.c:65` (`init_form`: `form->slot = 1;`), `:134` (`tui_get_int(... 1, 63);` allows slot 1)
- **Problem**: `init_form()` defaults `slot = 1`. The actuator dialog explicitly enforces "Slot must be >= 2 (Slot 1 reserved for CPU temp sensor)" at `dialog_actuator.c:459-462`. The wizard uses `SENSOR_SLOT_MIN = 2` (`dialog_io_wizard.c:47`). The sensor dialog has neither rule. Open Add Sensor, type a name, hit Save -- you get a UNIQUE constraint violation on slot 1, because the CPU temp sensor already owns it. The error is `dialog_error("Failed to save")` (line `339`). The user cannot tell from the message that the problem is the slot.
- **Scenario**: New tech opens Add Sensor for the first time. Doesn't realize they need to manually pick a slot. Types a name, picks a hardware type, hits Save. Cryptic "Failed to save" appears. They have no idea the slot is the issue. They retry with the same form. Same error. They give up and use the wizard instead.
- **Fix**: In `init_form`, set `form->slot` to the next free slot using `find_next_slot(true)` from the wizard module (extract it to a shared helper). Change `tui_get_int(..., &form->slot, 1, 63)` (line `134`) to `(..., &form->slot, 2, 246)` to match the actual valid PROFINET range and prevent slot 1. Catch the UNIQUE constraint failure in `save_sensor` and surface it as `"Slot N is already used by sensor 'foo'. Pick another slot."` instead of `"Failed to save"`.

---

### [CRITICAL] Sensor slot range is 1-63, but PROFINET range is 2-246

- **Location**: `src/tui/dialogs/dialog_sensor.c:134` (slot), `:135` (subslot)
- **Problem**: Sensor dialog hard-codes the slot range as `1-63`. The actual PROFINET range per `dialog_io_wizard.c:48` is `2-246`. A tech who knows the spec types in slot 100 and gets "Value out of range" with no useful explanation. They cannot use slots 64-246 from this dialog at all. Subslot is also constrained `1-63` (line `135`) -- again, narrower than PROFINET allows.
- **Scenario**: A site has 60 actuators and needs to add 4 more sensors. Slots 1-63 are already taken. Tech opens Add Sensor, picks slot 64. Dialog refuses. Tech is locked out of half the available slot space.
- **Fix**: Change to `tui_get_int(..., 2, 246)` for slot and `(..., 1, 32767)` (or whatever PROFINET subslot allows) for subslot. Reuse `SENSOR_SLOT_MIN`/`SENSOR_SLOT_MAX` from the wizard module rather than re-hardcoding numbers.

---

### [CRITICAL] Confirmation dialog defaults: `dialog_confirm` defaults to "No" (good), but `dialog_actuator_confirm_delete` has no default and accepts any keypress

- **Location**: `src/tui/dialogs/dialog_actuator.c:692-732` (`dialog_actuator_confirm_delete`)
- **Problem**: The actuator delete confirmation is a hand-rolled `getch()` loop at lines `722-731`. It accepts `Y/y` -> delete, `N/n/Esc` -> cancel, and *any other key* falls through and the loop runs again. There is no visual default selection, no Tab navigation, no Enter key handler at all. The instruction says "Press Y to confirm, N or Esc to cancel" -- if the tech hits Enter (the universal "I'm done" key), nothing happens. There is also no item-name double-confirmation: a tech who absent-mindedly hits 'y' in the wrong dialog deletes the wrong actuator. Compare with `dialog_confirm` (`dialog_helpers.c:175-217`) which at least defaults to "No" (line `187`) -- but `dialog_confirm` is what the *sensor* delete uses (`dialog_sensor.c:512`), not the actuator.
- **Scenario**: Pump house, flashlight, tech meant to refresh the actuator list (`r`) but typed `d` (delete). The confirmation pops up. Tech reflexively hits Enter -- nothing happens. Tech reflexively hits 'y' to dismiss the dialog. The actuator (probably a relay holding open an isolation valve) is now deleted and (per the previous critical) the GPIO line is never released, so the valve stays in whatever state it was.
- **Fix**: Replace `dialog_actuator_confirm_delete` with `dialog_confirm("Delete actuator?", msg)` (the existing helper) so the default is "No" and Enter selects the default. For *this* destructive action -- which can leave a relay energized due to the actuator-manager-reload bug above -- consider adding a typed confirmation: "Type the actuator name to confirm deletion".

---

### [HIGH] Sensor add dialog has no progressive disclosure: all 15 fields shown for every type

- **Location**: `src/tui/dialogs/dialog_sensor.c:78-113` (`draw_form`)
- **Problem**: The form draws all 15 fields regardless of `module_type`: Name, Slot, Subslot, Type, Hardware, Interface, Address, Bus, Channel, Gain, Ref Voltage, Unit, Min Value, Max Value, Poll Rate. For a `physical` DHT22 sensor, `Bus`, `Gain`, and `Ref Voltage` are meaningless. For an `adc` ADS1115, `Bus` and `Channel` are meaningful but they are also meaningful for an i2c physical sensor in a different way. The user has to fill in (or leave blank, hoping they're ignored) fields that have nothing to do with their device. Worst case: they fill in Channel `7` for a DHT22 thinking it matters, the value is silently dropped because the physical-sensor row doesn't have that meaning, and they wonder why the sensor doesn't read the channel they specified.
- **Scenario**: Tech adds a BME280 (i2c). They tab through Bus, Channel, Gain, Ref Voltage and don't know which apply. They fill in Bus=1, Channel=0, Gain=1, Ref Voltage=3.3 because they "look right". They save. The BME280 driver reads only address 0x76 from bus 1. Channel and Gain are ignored. Ref Voltage is ignored. The next tech reads the configuration in a year and wonders why this sensor has all those values set if they don't matter.
- **Fix**: After the user picks a Type (Physical / ADC / etc.), redraw the form showing only the fields that apply. Use a per-type field-mask. If using `dialog_io_wizard` for new sensors covers the common case (and it appears to), at minimum hide irrelevant fields in the *edit* dialog. The wizard does the right thing for adds; the edit dialog should not regress users to a dump-everything form.

---

### [HIGH] No first-field focus, no edit-mode title disambiguation by sensor name

- **Location**: `src/tui/dialogs/dialog_sensor.c:305` (`selected = 0`), `:407` (also `selected = 0`), `:312` (`Add Sensor` title), `:414` (`Edit Sensor` title)
- **Problem**: Both Add and Edit open with `selected = 0`, which is the Name field -- so first focus *is* reasonable for Add. For Edit, however, the title bar says only "Edit Sensor"; the actual sensor name is not in the title. The current name is shown in the Name field (which is correct), but the bold title bar -- the first thing the eye lands on -- gives no indication of *which* sensor you're editing. If a tech tabs away from the sensor list and comes back to a partially-completed Edit dialog, they have no header confirmation of which sensor they're modifying.
- **Scenario**: Tech opens Edit on `intake_pressure`. While reading the values, they go look at the controller for cross-reference. Come back to TUI ten minutes later. Title says "Edit Sensor". Was that intake_pressure or clearwell_level? They have to scroll the form to see the Name field. If multiple Edits were stacked (they aren't, but in a wizard chain it's plausible), they could be modifying the wrong one.
- **Fix**: Format the title as `Edit Sensor: intake_pressure (slot 5)`. If the title bar is too narrow, put it on a second line below the box. Always show the *immutable* identity of the thing being edited so the user can confirm "yes this is what I meant to open".

---

### [HIGH] No "unsaved changes" warning on Esc

- **Location**: `src/tui/dialogs/dialog_sensor.c:342, 350, 485, 493` (Esc -> immediate `return -1/false`), `dialog_actuator.c:570` (Esc -> `return false`)
- **Problem**: Hitting Esc in either the sensor or actuator dialog immediately discards all entered data with no prompt. There is no "you have unsaved changes, discard?" check. This is fine if the user was just browsing, but disastrous if they spent two minutes filling in a 15-field sensor and brushed Esc accidentally (Esc and `[` are adjacent on most keyboards; PuTTY sends Escape sequences via the same key).
- **Scenario**: Tech in PuTTY, slow ssh connection, fills in 14 fields for an ADS1115 channel. Their connection lags. They press Up arrow but the terminal sends `^[[A` (which is what arrow keys send). Some terminals briefly emit a bare `^[` first. The dialog catches Esc and exits immediately. Two minutes of work gone.
- **Fix**: Track a `dirty` flag set whenever any field is committed. On Esc, if dirty, prompt `dialog_confirm("Discard changes?", "...")` defaulting to No. If not dirty, exit silently as today.

---

### [HIGH] Hardware type for ADC dialog options exclude actually-supported types

- **Location**: `src/tui/dialogs/dialog_sensor.c:52-53` (`adc_hardware_types[]`), `:147-156`
- **Problem**: The ADC hardware list contains only `ADS1115` and `MCP3008`. The full hardware list includes `Generic`. The wizard supports `ADS1115` and `ADS1015` (`dialog_io_wizard.c:675, 1055`). The sensor dialog has no `ADS1015`, so a tech with an ADS1015 cannot configure it via the manual dialog -- only via the wizard. There is no message explaining this; the user must guess that "ADS1115" is close enough or that they have to use the wizard.
- **Scenario**: Tech has a board with an ADS1015. They open Add Sensor (not the wizard, because they want to control fields directly), pick adc, look at hardware list, see only `ADS1115` and `MCP3008`. They pick ADS1115. The driver may or may not work with the ADS1015 chip depending on register compatibility. Silent miscategorization.
- **Fix**: Add `ADS1015` to `adc_hardware_types[]`. Audit the list against `src/drivers/` to find any other supported ADC chips that are missing.

---

### [HIGH] Hardware type for "physical" splits out ADS/MCP but doesn't reflect interface constraints

- **Location**: `src/tui/dialogs/dialog_sensor.c:56-61` (`physical_hardware_types`), `:167-173` (Address field as free text)
- **Problem**: A physical sensor's `address` is a free-text field. For DS18B20, address is the 1-Wire ID (`28-XXXXXXXX...`). For BME280 it's an i2c address (`0x76`). For DHT22 it's a GPIO pin number. There is no inline help, no example, no per-type validation. After picking the hardware type, the address field accepts anything. The format is implicit and undocumented in the dialog.
- **Scenario**: Tech adds a DS18B20 manually. The wizard would fill in the 1-wire ID automatically; but the manual dialog asks for it as text. Tech types `28a3b1c4...` -- did they capitalize the right letters? Did they include the family byte? They have no way to verify until they save and watch the sensor fail to read.
- **Fix**: After hardware-type selection, change the Address field's prompt and validation per type: for DS18B20, scan `/sys/bus/w1/devices/` and present a picklist. For i2c sensors, present a hex address field with `0x` prefix and validate `0x03..0x77`. For DHT22, treat as integer GPIO pin and validate against board GPIO range. Show the expected format inline as ghost text under the field. The wizard already does this kind of discovery -- the manual dialog should reuse the same helpers, not regress.

---

### [HIGH] Actuator dialog: hardcoded fallback GPIO pins document themselves as "RPi-only"

- **Location**: `src/tui/dialogs/dialog_actuator.c:114-122` (`show_pin_selector` fallback)
- **Problem**: When `board_detect()` returns no pins, the fallback list is `{17, 27, 22, 23, 24, 25}` -- Raspberry Pi GPIO numbering. The codebase explicitly notes (per CLAUDE.md) that Odroid-XU4 has `gpiochip0..35` and very different pin numbers. A tech on an Odroid sees pin numbers that may not exist on their board. The CLAUDE.md acknowledges this fallback is "intentional design choice" but does not warn the user.
- **Scenario**: Tech on Odroid-XU4, board detection fails (the unit is a new SKU and not in the board table). They open Add Actuator manually, the pin selector pops up showing GPIO 17/27/22/23/24/25. They pick 17. Save. The actuator manager tries to claim line 17 from the wrong gpiochip and fails. Tech gets no error in the dialog -- the failure happens in the actuator manager (which is never reloaded anyway, per the earlier critical).
- **Fix**: When falling back, show a warning banner in the pin selector: "Board not detected -- showing common pins. Verify against your board datasheet." Or refuse to fall back at all and force the user to enter a manual pin number with a "you must verify this pin exists on your board" warning. Better: when board_detect fails, try to enumerate gpiochips at runtime via `/sys/class/gpio/` and show the highest pin counts available.

---

### [HIGH] Actuator dialog: `gpio_chip` is a free-text field

- **Location**: `src/tui/dialogs/dialog_actuator.c:323-333` (display), `:432, 472` (read/write `edit_buffer`)
- **Problem**: `gpio_chip` is editable as a free-text field. Tech can type `gpiochip99` and the dialog will accept it. The actual error happens later, in the actuator manager, with no UI feedback (and per the earlier critical, the actuator manager isn't even reloaded after save).
- **Scenario**: Tech misreads the docs and types `gpiochip1` when they should have left it `gpiochip0`. Save succeeds. Actuator never works. There is no validation step.
- **Fix**: Change `gpio_chip` to a select field, populated by enumerating `/dev/gpiochip*` at dialog open time. If only one chip exists, hide the field entirely (the wizard does this implicitly via board_detect). Validate against the actual filesystem.

---

### [HIGH] Actuator dialog: min/max on-time accepts negative values and inconsistent ordering

- **Location**: `src/tui/dialogs/dialog_actuator.c:478-483` (commit), `:373-400` (display)
- **Problem**: `min_on_time_ms` and `max_on_time_ms` are committed via raw `atoi()`. There is no validation that:
  - `min_on_time_ms >= 0`
  - `max_on_time_ms >= 0`
  - `min_on_time_ms <= max_on_time_ms` (when max > 0)
  
  A tech can set min=5000, max=1000, save, and the actuator manager will be configured with an impossible safety constraint. There is no inline range hint.
- **Scenario**: Tech configures a chemical dosing pump. Wants minimum 100 ms (to debounce), maximum 5000 ms (to prevent overdose). Mis-types: enters min=5000, max=100. The dialog accepts it. The dosing pump will either never fire (min not satisfied) or fire dangerously (max enforcement broken).
- **Fix**: On commit, validate `min >= 0`, `max >= 0`, and (`max == 0` or `min <= max`). Reject with a specific error: `"Min on time (5000ms) cannot exceed max on time (100ms)"`. Show the range in the field hint: `Min On (ms) [0..max_on_time]`.

---

### [HIGH] Actuator dialog draws on stdscr, not on a window -- breaks under terminal resize

- **Location**: `src/tui/dialogs/dialog_actuator.c:227-414` (`draw_dialog` uses `mvprintw`/`mvhline`/`attron` -- the stdscr versions, not `mvwprintw`)
- **Problem**: Unlike `dialog_sensor` (which creates a `WINDOW *dialog` and uses `mvwprintw`), `dialog_actuator` draws directly on stdscr. There is no associated `WINDOW *` to refer to for input via `wgetch(win)` -- the input uses bare `getch()` (line `685`). This means:
  1. If the user resizes the terminal during the dialog, no `KEY_RESIZE` handler exists and the dialog draws garbage onto the resized stdscr.
  2. The background page (sensor list, status, etc.) is permanently overwritten in the regions the dialog drew. When the dialog exits there is no "behind-the-modal" content to restore.
  3. There is no `keypad(stdscr, TRUE)` guarantee inside the dialog, so arrow-key parsing depends on the parent state.
- **Scenario**: Tech on a small terminal opens Edit Actuator. Realizes they can barely read it. Resizes the terminal window larger. The dialog drawing is now smeared across stale rows. Tech has to Esc out and reopen.
- **Fix**: Refactor `dialog_actuator` to use `dialog_create()` (the same helper `dialog_sensor` uses), draw via `mvwprintw`, and add `case KEY_RESIZE` handling in `handle_input` that calls `endwin(); refresh(); clear();` and re-draws the dialog at the new center.

---

### [HIGH] Wizard: no resize handling either

- **Location**: `src/tui/dialogs/dialog_io_wizard.c:1858-1862` (window created at fixed `WIZARD_WIDTH=70 WIZARD_HEIGHT=20`), `:683-687, :812-859, ...` (every screen handler reads `wgetch(g_wiz.win)` with no `KEY_RESIZE` case)
- **Problem**: The wizard creates a fixed-size centered window. None of the screens handle `KEY_RESIZE`. When the terminal is resized mid-wizard, the window stays at its old position and the underlying screen redraws around it leave artifacts. Combined with the fact that the wizard has no save/restore between screens (push_state preserves state, but the window object is fixed), a resize during a long wizard run either crashes or leaves visual garbage.
- **Scenario**: Tech opens the IO wizard on a 80x24 terminal, decides to enlarge to 120x40 mid-wizard for readability. Wizard window stays in the old 80x24 center. New terminal area is filled with old page content underneath. Tech can't tell where the wizard ends and the page begins.
- **Fix**: Add `case KEY_RESIZE` to every wizard screen handler (or wrap the `wgetch` in a helper that catches resize, recreates the window centered on the new size, and re-renders the current state).

---

### [HIGH] Wizard: no "Save" branch implemented for "Edit Advanced" button

- **Location**: `src/tui/dialogs/dialog_io_wizard.c:1655-1658` (Confirm screen, "Edit Advanced" button)
- **Problem**: The Confirm screen has three buttons: `Save`, `Edit Advanced`, `Cancel`. Selecting "Edit Advanced" pops up `dialog_message("Advanced", "Advanced settings not yet available.")` and stays on the confirm screen. The button looks operational but is a placeholder. A tech who wants to set, say, a custom poll rate or a PWM frequency before saving has no path forward except Save-then-Edit (which is the broken-on-GPIO edit dialog flagged earlier).
- **Scenario**: Tech adds a chemical dosing pump via the wizard. On the Confirm screen, tech wants to lower the PWM frequency from 1000 Hz to 100 Hz before saving. Picks Edit Advanced. Gets "not yet available". Now they have to Save with default 1000 Hz, then immediately re-open the (broken) Edit dialog -- which only works if the actuator has no GPIO conflict, which it does, with itself.
- **Fix**: Either remove the "Edit Advanced" button entirely until it's wired up, or make it open the existing `dialog_actuator_show` (in EDIT mode pre-populated from the wizard's draft) for actuators. For sensors, hand off to a sensor-specific advanced form.

---

### [HIGH] Wizard auto-set safe defaults are invisible at confirm time

- **Location**: `src/tui/dialogs/dialog_io_wizard.c:1801-1842` (`save_actuator`), `:1604-1610` (Confirm screen actuator summary)
- **Problem**: `save_actuator` hard-codes `safe_state = SAFE_STATE_OFF`, `enabled = true`, `pwm_frequency_hz = 1000`, `active_low = false`. The Confirm screen shows only `"Safe State: OFF (de-energize on fault)"` -- it does *not* tell the user about active_low default, PWM frequency, or that the actuator will be `enabled` immediately. For an `active_low` relay (most opto-isolated relay boards are wired this way), the wrong default means the relay energizes the *opposite* of what the controller intends. The tech would need to know this in advance and use Edit afterward to flip it, but Edit is broken for GPIO sensors and uncertain for actuators.
- **Scenario**: Tech wires a "active LOW" relay board to GPIO 22 to control a chlorine dosing pump. Runs the wizard. Defaults `active_low = false`. Saves. The relay is now energizing the dosing pump every time the controller writes "off" and de-energizing every time it writes "on". The tech only finds out when they smell chlorine in the next room.
- **Fix**: Either prompt for `active_low` explicitly in the wizard (one screen, two options: "active high (5V on)" or "active low (5V off / opto-isolated)"), or default `active_low` based on detected board (most boards have a known default), or at least display the default on the Confirm screen so the tech sees it before committing. For relays specifically, this is a safety-critical default.

---

### [HIGH] Sensor add: subslot defaults to 1 with no explanation

- **Location**: `src/tui/dialogs/dialog_sensor.c:66`
- **Problem**: `subslot = 1` is set as a default, but the user has no idea what subslot is for or whether to change it. The PROFINET subslot is rarely meaningful for simple sensors but is exposed as a top-level form field. There's no help text. Tech may be tempted to set it to "their device address" or "the rack number" or any other plausible-but-wrong value.
- **Scenario**: Tech is adding 8 channels of an ADS1115. They see `Subslot` and assume that's the channel index. They set subslot=0 for channel 0, subslot=1 for channel 1, etc. The PROFINET stack now has 8 different subslot values that none of the controller logic uses. Worse, if they set subslot to a value that conflicts with an existing entry, they get a UNIQUE-style error that is confusing because the slot is unique but they thought subslot was the differentiator.
- **Fix**: Hide subslot from the form for simple sensors (always 1). Only expose subslot in an Advanced section that is collapsed by default. Add inline help: "Subslot is for compound modules; leave at 1 unless you know you need otherwise."

---

### [MEDIUM] Sensor delete confirmation: identifies sensor by name+slot but not by type

- **Location**: `src/tui/dialogs/dialog_sensor.c:509-510`
- **Problem**: The delete confirmation reads `"Delete 'foo' (slot 5)?"`. It does not show the type, hardware, or current value. Two sensors with similar names (e.g., `tank_a_level` and `tank_b_level`) at different slots are indistinguishable from a quick read. The tech sees `'tank_a_level' (slot 5)` and may not realize they're on slot 5 vs slot 6.
- **Scenario**: Two pH sensors named `ph_clearwell_1` and `ph_clearwell_2`. Tech wants to delete the broken one. Delete dialog says `"Delete 'ph_clearwell_1' (slot 5)?"`. Tech is reading by name, not by slot, and doesn't notice the slot. Hits Yes. Wrong sensor deleted.
- **Fix**: Expand the message: `"Delete sensor 'ph_clearwell_1'?\n  Type: ADS1115 channel 0\n  Slot: 5\n  Last value: 7.23 pH"`. Show enough context that the tech can confirm it's the right one before clicking Yes.

---

### [MEDIUM] Wizard "name" screen requires lowercase + underscores; rejects spaces and hyphens

- **Location**: `src/tui/dialogs/dialog_io_wizard.c:451-468` (`validate_name`)
- **Problem**: Names must match `^[a-z0-9_]+$`. The error is "Use lowercase letters, numbers, underscores only". The wizard's example text says `intake_pressure, clearwell_level`. The convention is fine for internal identifiers, but the user has no way to put a human-readable name on the sensor for the dashboard. Hyphens are forbidden, so `tank-a-1` becomes `tank_a_1` which is less natural for many techs.
- **Scenario**: Tech wants to call it `Intake Pressure (NW)`. Validation rejects. They retry `intake_pressure_nw`. It works but feels arbitrary -- and they wonder why other dialogs and parts of the system that use names elsewhere allow spaces.
- **Fix**: Either relax the validation to accept hyphens and a leading capital (still no spaces, for shell-safety), or keep the strict validation but add a separate `display_name` field that allows free text. The strict identifier becomes the slot key; the display name is what shows up in the list. Make the example reflect: `Examples: intake_pressure, ph_probe_1`.

---

### [MEDIUM] Wizard "name" screen has no way to edit name in place; user must press Enter to open another dialog

- **Location**: `src/tui/dialogs/dialog_io_wizard.c:1534-1554`
- **Problem**: The Name screen shows the current name and instructs `[Enter] Edit name`. Pressing Enter opens *another* `dialog_input_string` modal. Two-modal-deep for a single text field. The tech has to type into the inner modal, then accept, then they're back at the Name screen, then they must press Enter *again* -- not to advance, but to validate-and-go-to-confirm? Actually re-reading, after `validate_name` succeeds, `push_state(WIZ_STATE_CONFIRM)` advances. So one Enter to open the input, one set of typing, one Enter to commit it = goes to Confirm. That's two Enters and two distinct UIs for what should be one field.
- **Scenario**: Tech is fast-typing through the wizard. On the Name screen, they expect to just type the name. Nothing happens because it's a display screen. They press Enter, a dialog pops up. They type. They press Enter. They're back at the Name screen for a brief flash, then onto Confirm. The flicker is disorienting.
- **Fix**: Make the Name screen edit in place. The screen already has a colored input area drawn (line `1517-1519`). Use `tui_get_string()` directly there instead of opening a separate dialog.

---

### [MEDIUM] Wizard ADC channel pick: no "what's connected" preview

- **Location**: `src/tui/dialogs/dialog_io_wizard.c:1080-1162`
- **Problem**: The ADC channel picker shows `Channel 0 (A0) [available]`, `Channel 1 (A1) [in use: ph_probe]`, etc. It does NOT show the *current voltage* on the channel. For a physical sensor, the wizard's main scan screen helpfully shows the live DS18B20 reading (line `711-716`). The same affordance is not present for ADC channels. A tech has no way to verify "is the wire I just plugged in actually connected to A2?" without saving and waiting for the read loop to start.
- **Scenario**: Tech crimps a pH probe to A2 of an ADS1115 and wants to verify continuity before configuring. They run the wizard. Pick A2. The wizard offers no live reading. They save. They go to the sensor list. They wait for the value to update. If the wire is loose, they only find out after the full config cycle.
- **Fix**: When showing the ADC channel picker, do a one-shot read of each channel and display the raw mV alongside the channel number. Tech sees A0=0.001V, A1=2.13V, A2=4.20V, A3=0.00V, knows A2 is the live one without committing.

---

### [MEDIUM] No keyboard shortcut help on the sensor edit dialog

- **Location**: `src/tui/dialogs/dialog_sensor.c:117-119`
- **Problem**: The hint line at the bottom of the sensor dialog says only `Up/Down: Navigate  Enter: Edit/Select` and `Tab: Buttons  Esc: Cancel`. There is no Save shortcut, no Help key, no field-specific tip. Compare with the actuator dialog (`dialog_actuator.c:411`) which mentions `F10: Save` -- a much faster path than tabbing to the button row.
- **Scenario**: Tech edits a sensor, makes a single field change, wants to save. They have to Tab to the button row, then Enter on Save. With F10 they could be done in one keystroke. They never discover this because the hint doesn't mention it.
- **Fix**: Add `F10: Save` to the sensor dialog and to the wizard confirm screen, and document it in the bottom hint line. Also mention `F1: Help` (and implement a per-field help popup).

---

### [MEDIUM] No "duplicate this sensor" / "copy as new" affordance

- **Location**: `src/tui/pages/page_sensors.c:131` (`help_text`), `src/tui/pages/page_actuators.c:228` (help line)
- **Problem**: There is no duplicate-sensor / clone-as-new keybinding. To configure 4 channels of an ADS1115 (a common task) you have to walk through Add Sensor four times, re-typing all the shared fields each time. The wizard helps for the auto-discovery case but doesn't pre-populate from an existing sensor.
- **Scenario**: Tech configures 8 ADC channels for a pump bank. Each one has the same hardware, bus, address, gain, ref voltage, unit -- only the channel number and name differ. They walk through 8 Add dialogs, each with 15 fields. Two minutes per sensor times 8 = 16 minutes of repetitive entry. One mistyped Bus value silently ruins one of the 8.
- **Fix**: Add a "Duplicate" key (e.g., `c` for clone) to the sensor and actuator list pages. Pre-fill an Add dialog with the selected item's fields, with the slot auto-incremented to the next free slot. Tech edits only the channel number and name. Saves. Same for actuators.

---

### [MEDIUM] Page-level keybinding for delete is `d`, but the help line says `Del` is also accepted

- **Location**: `src/tui/pages/page_actuators.c:401-402` (`case 'd': case KEY_DC:`)
- **Problem**: Actuator delete responds to both `d` and the Delete key (`KEY_DC`). The help line says only `d:Delete`. Sensor page (`page_sensors.c:131`) says `d:Delete` too. The Delete key is undocumented. This is a small discoverability issue but worth noting -- the "real" Delete key on most keyboards is what a non-power-user would reach for.
- **Fix**: Document `Del` in the help text alongside `d`. Or: pick one, document one. Consistency wins.

---

### [MEDIUM] Wizard fallback GPIO pin list is hardcoded RPi pins, not Odroid

- **Location**: `src/tui/dialogs/dialog_io_wizard.c:911-920` (sensor GPIO fallback), `:1335-1345` (actuator GPIO fallback)
- **Problem**: When `board_detect()` returns no input/relay pins, the wizard falls back to `{17, 27, 22, 5, 6}` for sensors and `{17, 27, 22, 23}` for actuators. These are Raspberry Pi pin numbers. As noted in the actuator dialog finding above, an Odroid-XU4 has different GPIO numbering. The wizard does not warn about this.
- **Fix**: Same as the actuator dialog finding: warn the user explicitly when in fallback mode, or detect available gpiochips at runtime and offer pins from those.

---

### [MEDIUM] Sensor/actuator add: no "in this terminal width" overflow guard

- **Location**: `src/tui/dialogs/dialog_sensor.c:19-20`, `dialog_actuator.c:228-229`
- **Problem**: Sensor dialog is `WIDTH 65 HEIGHT 22`. Actuator dialog is `WIDTH 60 HEIGHT 18`. Both are sized for 80x24 *exactly*. There is no guard against terminals smaller than 80x24 (some embedded SoCs default to 40x16 on serial console). On a too-small terminal, the dialog will silently draw outside the screen and break.
- **Fix**: Check `getmaxyx(stdscr, ...)` at dialog open, and either refuse to open with a "terminal too small" error or fall back to a single-column layout for narrow terminals.

---

### [LOW] Save-failure message is generic ("Failed to save", "Failed to update module")

- **Location**: `src/tui/dialogs/dialog_sensor.c:339, 441`
- **Problem**: Errors from the DB layer are reported as `"Failed to save"` or `"Failed to update module"`. The DB layer logs the actual sqlite error to the log file, but the TUI user only sees the generic message.
- **Fix**: Pass the underlying error string up via a thread-local error buffer, or at minimum special-case the most common: UNIQUE violation -> "Slot N is already in use". NOT NULL violation -> "Required field missing". Disk full -> "Disk full -- cannot save".

---

### [LOW] Inconsistent button defaults across destructive dialogs

- **Location**: `src/tui/dialogs/dialog_helpers.c:187` (defaults to "No"), `dialog_actuator.c:692-732` (no default), wizard `screen_confirm` defaults to "Save" (`dialog_io_wizard.c:1628`)
- **Problem**: `dialog_confirm` defaults Yes/No to "No". `dialog_actuator_confirm_delete` has no concept of default. Wizard confirm screen defaults to "Save" (the destructive option from a "discard work" angle, but not destructive to existing data). The behavior is inconsistent and a tech who has built a habit on one dialog will mis-press another.
- **Fix**: Pick a project-wide rule: destructive actions (delete) default to Cancel/No. Constructive actions (save new) default to Save. Apply consistently.

---

### [LOW] No visible "RTU state" indicator in the dialogs

- **Location**: All dialog files
- **Problem**: When the tech is editing a sensor, there is no visible indication of whether PROFINET is in STOP, RUN, or no-AR. A change saved during RUN may behave differently than one saved during STOP -- the controller may immediately push new IOPS, the connection may renegotiate, etc. The tech has no awareness.
- **Fix**: Show the PROFINET state in the dialog title bar (e.g., `Edit Sensor [PROFINET: RUN]`) or in a corner of the dialog. If the state is RUN, show a subtle warning: "Changes will apply on next AR connect" or "Live update -- controller may see change immediately depending on type".

---

### [LOW] Sensor dialog uses A_REVERSE for selection, no color contrast

- **Location**: `src/tui/dialogs/dialog_sensor.c:85, 107`
- **Problem**: Selection is indicated by `A_REVERSE` only, with no additional color. On terminals with poor reverse-video support (some serial consoles, some embedded TTYs), this can be hard to see. Consistency with other parts of the UI is fine but worth documenting.
- **Fix**: Pair `A_REVERSE` with a color attribute (e.g., `COLOR_PAIR(TUI_COLOR_INPUT)`) so contrast is doubled. Add an arrow `>` indicator on the selected row as a non-color cue (the wizard already does this at `dialog_io_wizard.c:312`).

---

### [LOW] Wizard navigation contract: ESC always-back, but on the very first screen Esc cancels

- **Location**: `src/tui/dialogs/dialog_io_wizard.c:557-559` (screen_io_type Esc handler)
- **Problem**: The header doc says "ESC always goes back exactly one step" (`dialog_io_wizard.c:14`). On the first screen, there is nowhere to go back to, so Esc cancels. This is the right behavior, but it's a soft inconsistency with the doc -- a tech who has internalized "Esc = back" will eventually hit it on screen 1 and cancel.
- **Fix**: On screen 1, prompt before exiting: "Cancel adding new I/O?". Or change the doc to "ESC: back, or cancel from the first screen".

---

### [LOW] No way to configure poll rate from the wizard

- **Location**: `src/tui/dialogs/dialog_io_wizard.c:1603` (`Poll Rate: 1000 ms` shown as fixed in confirm)
- **Problem**: The wizard hardcodes `poll_rate = 1000 ms` for all created sensors. The Confirm screen shows it as a label, not a field. Tech who wants 100 ms or 5000 ms must Save then Edit. Edit is broken for GPIO sensors. So in practice they can never adjust.
- **Fix**: Once "Edit Advanced" is implemented (per the earlier finding), poll rate is the obvious first thing to expose. Or: add a single screen at the end that lets the user pick `Slow / Default / Fast / Custom` for poll rate.

---

### [LOW] Sensor dialog hint says "Tab: Buttons" but Tab also exits the form area

- **Location**: `src/tui/dialogs/dialog_sensor.c:118, 348, 491`
- **Problem**: Tab in the sensor dialog moves to the button row. There is no way to Shift-Tab back to the form, and there is no way to Tab between fields (Up/Down does that). Most users expect Tab to move between form fields.
- **Fix**: Either rebind Tab to next-field and Shift-Tab to previous-field, or document the existing behavior more clearly: "Tab: Move to buttons. Up/Down: Move between fields."

---

## Summary

**8 critical, 12 high, 9 medium, 6 low.**

### Top 3 CRITICAL findings restated

1. **Editing a GPIO sensor is impossible.** `dialog_sensor.c:222` ignores `exclude_sensor_id`, so any sensor with `interface = "gpio"` (DHT22, Float Switch, etc.) finds itself in the GPIO conflict check and refuses to save. Tech is forced to delete-and-re-add to make any change.
2. **Actuator add/edit/delete never reloads the actuator manager.** `page_actuators.c` and `dialog_actuator.c` make no `actuator_manager_reload()` call. The DB is updated but the running actuator manager keeps its old state -- old GPIO claims, old PWM channels, old instances. Manual control through the TUI for newly-added actuators fails silently. Deleted actuators may leave their relay energized.
3. **Sensor types `web_poll`, `calculated`, `static` are silently dropped on save.** `dialog_sensor.c:264-293` only handles `physical` and `adc`. Selecting any other type creates a `modules` row with no sub-record. The sensor appears in the list but never reads. There is no error.

### Recommendations for the add-sensor flow specifically

The right entry point is the `dialog_io_wizard` -- it has progressive disclosure, hardware discovery, conflict detection, and reasonable defaults. The page-level callback already routes Add to the wizard (`page_sensors.c:212`), which is correct. The remaining work for the add-sensor flow is:

1. **Fix the wizard's "Edit Advanced" button** so the tech can adjust poll rate, ranges, and units before saving (`dialog_io_wizard.c:1655`). Most field-tech complaints will be about the inability to set an unusual poll rate.
2. **Implement live channel preview** for ADC selection (`dialog_io_wizard.c:1080-1162`) so the tech can confirm a wire is connected to A2 before committing.
3. **Make the wizard auto-detect `active_low`** based on board profile, or prompt for it explicitly. Defaulting to `active_low = false` is safety-critical for relays.
4. **Allow display-friendly names** in addition to the strict-identifier name (`dialog_io_wizard.c:451-468`).
5. **Add a "duplicate this sensor" key** to the sensor list (`page_sensors.c`) so configuring an 8-channel ADC bank is not an 8x repetitive walk.

The standalone `dialog_sensor` Add path should either be removed (it's not the recommended entry) or fixed comprehensively: real progressive disclosure, real validation, slot defaults that don't collide with reserved slot 1, and the missing sensor-type branches.

### Recommendations for the add-actuator flow specifically

1. **CRITICAL: wire `actuator_manager_reload(&g_actuator_mgr)` into both the wizard save (`dialog_io_wizard.c:1842`) and the dialog save (`page_actuators.c:391`).** Without this, every add and edit is a no-op for the running actuator manager. Without this fix, no other UX improvement matters.
2. **Replace `dialog_actuator_confirm_delete` with `dialog_confirm`** so Esc and default-No semantics are consistent (`dialog_actuator.c:692-732`).
3. **Refactor `dialog_actuator` to use a real `WINDOW *`** via `dialog_create()` so it can survive a terminal resize (`dialog_actuator.c:227-414`).
4. **Validate min/max on-time at commit** (`dialog_actuator.c:478-483`) so impossible safety constraints can't be saved.
5. **Make `gpio_chip` a select field** populated from `/dev/gpiochip*` (`dialog_actuator.c:323-333`) so a typo can't break the actuator silently.
6. **Show defaults on the wizard confirm screen** (`dialog_io_wizard.c:1604-1610`) so the tech sees `active_low`, `enabled`, and `pwm_frequency_hz` before committing -- not just "Safe State: OFF".

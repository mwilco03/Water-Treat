# RTU Team -- p-net / PROFINET Technical Answers

Answers derived from codebase analysis with code references.

---

## 1. Does your p-net build require the 20-byte NDR header?

**No.** The RTU does not construct NDR headers. p-net v0.2.0 (rt-labs) handles all DCE/RPC framing internally. The application layer interacts only through `pnet_api.h` callbacks.

The 20-byte stub in the captured response (frame 127 in `new.txt`) is p-net's **IODConnectRes error block**, not a manually-constructed NDR header:

```
Frame 127: Connect response
  Fragment len: 20
  Complete stub data (20 bytes)
  Error: "IODConnectRes", "PNIO", "Connect: Faulty ARBlockReq",
         "Error in Parameter BlockLength"
```

The request from the controller (frame 126) contains 712 bytes of stub data with ARBlockReq + IOCRBlockReq + ExpectedSubmoduleBlockReq. p-net parses and rejects this internally -- the application `connect_callback` may or may not fire depending on how early the rejection occurs.

**Key file:** `src/profinet/profinet_manager.c:861-875` -- p-net initialization, no RPC layer interaction.

---

## 2. Does p-net read UUIDs per DREP or hardcode one encoding?

**p-net handles UUID marshaling internally.** The application code never touches UUIDs.

From the pcap (`new.txt` frame 126):

```
Object UUID:    88b02394-3f89-bf2a-a533-82c617832c7d
Interface UUID: dea00001-6c97-11d1-8271-00a02442df7d  (standard PNIO Device)
Activity UUID:  ec8cbbfd-019b-0040-a91f-8e23cd7ce846
```

The Interface UUID `dea00001-6c97-11d1-8271-00a02442df7d` is the standard PROFINET Device interface UUID per IEC 61158-6. p-net reads the DREP field to determine byte order:

| Frame | Direction | DREP | Byte Order |
|-------|-----------|------|------------|
| 126 (Request) | Controller -> RTU | `100000` | Little-endian |
| 127 (Response) | RTU -> Controller | `000000` | Big-endian |

The only GSDML UUID reference is `ObjectUUID_LocalIndex="1"` (`gsd/GSDML-V2.4-WaterTreat-RTU-20241222.xml:58`) -- a schema index, not a wire UUID.

---

## 3. Is DREP=0x00 in responses intentional (p-net default)?

**Yes.** p-net v0.2.0 uses big-endian (network byte order) for all response encoding. This is the library default, not something the application configures.

The RTU application code is consistently big-endian throughout:

```c
// profinet_manager.c:1087-1093
uint32_t raw;
memcpy(&raw, &value, sizeof(raw));
uint32_t be = htonl(raw);
memcpy(data, &be, sizeof(be));
```

```c
// user_sync.c -- network byte order conversion
uint32_t user_id = ntohl(entry->user_id);
uint16_t stored_checksum = ntohs(hdr->checksum);
```

The DREP mismatch (request=LE, response=BE) is **normal DCE/RPC behavior** -- each side advertises its own native encoding. This is not the cause of the "Faulty ARBlockReq" error.

---

## 4. What slots/modules are actually plugged in the test RTU?

Slot configuration is **dynamic**, loaded from the SQLite database at startup.

### Always plugged -- DAP (slot 0)

From `profinet_manager.c:885-925`:

| Slot | Subslot | Module Ident | Submodule Ident | Purpose |
|------|---------|-------------|-----------------|---------|
| 0 | 1 | 0x00000001 | 0x00000001 | DAP |
| 0 | 0x8000 | 0x00000001 | 0x00000100 | Interface |
| 0 | 0x8001 | 0x00000001 | 0x00000200 | Port |

### Database-driven application modules

From `profinet_manager.c:371-410` and `gsdml_modules.h`:

| Module | Ident | Submod | Direction | Size |
|--------|-------|--------|-----------|------|
| pH Sensor | 0x00000010 | 0x00000011 | INPUT | 5 bytes |
| TDS Sensor | 0x00000020 | 0x00000021 | INPUT | 5 bytes |
| Turbidity | 0x00000030 | 0x00000031 | INPUT | 5 bytes |
| Temperature | 0x00000040 | 0x00000041 | INPUT | 5 bytes |
| Flow | 0x00000050 | 0x00000051 | INPUT | 5 bytes |
| Level | 0x00000060 | 0x00000061 | INPUT | 5 bytes |
| Generic AI | 0x00000070 | 0x00000071 | INPUT | 5 bytes |
| Pump | 0x00000100 | 0x00000101 | OUTPUT | 4 bytes |
| Valve | 0x00000110 | 0x00000111 | OUTPUT | 4 bytes |
| Generic DO | 0x00000120 | 0x00000121 | OUTPUT | 4 bytes |

Sensor input format: 4-byte IEEE 754 float (BE) + 1-byte quality.
Actuator output format: 1-byte command + 1-byte duty + 2-byte reserved.

### Empty database warning

If the database has no modules configured, only DAP is plugged (`profinet_manager.c:1471-1476`):

```c
if (g_pn.slot_count == 0) {
    LOG_WARNING("No application modules plugged (only DAP).");
    LOG_WARNING("Controller expects slots 1-%d but RTU has none configured.",
                MAX_PROFINET_SLOTS - 1);
}
```

**This is likely the root cause of "Faulty ARBlockReq"** -- the controller's `ExpectedSubmoduleBlockReq` lists modules the RTU hasn't plugged.

---

## 5. Do you have a pcap of a successful connection with any other controller?

**No successful connection capture exists.** Two captures are present:

1. **`new.txt`** -- Wireshark text dissection of a **failed** connection:
   - Frames 44-45: DCP Identify (succeeds -- RTU responds as `rtu-4b64`, Vendor 0x0493)
   - Frame 126: Connect Request from `192.168.6.13` (712-byte stub)
   - Frame 127: Connect Response with `"Faulty ARBlockReq", "Error in Parameter BlockLength"`

2. **`profi.pcapng`** -- 1.7 KB binary pcapng at repo root (not yet analyzed as text)

DCP discovery works. The failure is at the RPC Connect level, inside p-net's ARBlockReq parsing. The debug message at `profinet_manager.c:991` confirms:

```c
LOG_INFO("If connect_callback is NOT called, p-net rejects before app layer");
```

---

## 6. What p-net version and any custom RPC modifications?

**p-net v0.2.0, zero custom RPC modifications.**

From `scripts/install-deps.sh:562-564`:

```bash
# Pin to v0.2.0 - the last version with CMakeLists.txt in the root
# Later versions removed CMake support from the public repository
local pnet_version="v0.2.0"
```

- **Source:** `https://github.com/rtlabs-com/p-net.git`
- **Installed as:** `libprofinet.so` with symlink `libpnet.so`
- **Headers:** `/usr/local/include/pnet_api.h`

Two files `pf_cmrpc.c` and `pf_cmdev.c` exist at the repo root but are 14-byte placeholder stubs containing `"404: Not Found"` -- not custom RPC code.

All integration is through standard `pnet_api.h` callbacks (`profinet_manager.c:461-474`):

```c
g_pn.pnet_cfg.state_cb       = profinet_state_callback;
g_pn.pnet_cfg.connect_cb     = profinet_connect_callback;
g_pn.pnet_cfg.release_cb     = profinet_release_callback;
g_pn.pnet_cfg.dcontrol_cb    = profinet_dcontrol_callback;
g_pn.pnet_cfg.ccontrol_cb    = profinet_ccontrol_callback;
g_pn.pnet_cfg.read_cb        = profinet_read_callback;
g_pn.pnet_cfg.write_cb       = profinet_write_callback;
g_pn.pnet_cfg.exp_module_cb  = profinet_exp_module_callback;
g_pn.pnet_cfg.exp_submodule_cb = profinet_exp_submodule_callback;
// ... alarm, reset, LED callbacks
```

---

## 7. Plans for a PI-registered vendor ID vs keeping rt-labs' 0x0493?

**Currently using 0x0493 with an explicit "should be registered" comment.**

From GSDML (`gsd/GSDML-V2.4-WaterTreat-RTU-20241222.xml:12`):

```xml
Vendor ID: 0x0493 (example - should be registered with PI)
```

Configured in four locations:

| File | Value | Context |
|------|-------|---------|
| `include/config_defaults.h:42` | `#define WT_PROFINET_VENDOR_ID 0x0493` | Compile-time default |
| `gsd/GSDML-*.xml:41` | `VendorID="0x0493"` | GSDML device identity |
| `src/profinet/profinet_callbacks.c:61` | `.vendor_id = 0x0493` | I&M0 data block |
| `etc/water-treat.conf.example:22` | `vendor_id = 0x0493` | Runtime config override |

The vendor is identified as `"Water Treatment Training"` in the GSDML.

**Note:** 0x0493 is rt-labs' registered PI vendor ID. Using it in production would conflict with legitimate rt-labs devices on the same PROFINET network. The architecture supports runtime override via the config file (`[profinet] vendor_id`), so switching to a registered ID requires updating the GSDML, the compile-time default, and the I&M0 static initializer.

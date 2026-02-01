/**
 * @file profinet_identity.h
 * @brief Single source of truth for PROFINET device identity
 *
 * These values MUST match the GSDML file:
 *   gsd/GSDML-V2.4-WaterTreat-RTU-20241222.xml
 *
 * Both RTU (Water-Treat) and Controller (Water-Controller) must use
 * identical identity values. The controller should fetch this file from
 * the Water-Treat repo (RTU is the source of truth for device identity).
 *
 * If you change ANY value here, you MUST also update the GSDML XML.
 * The GSDML is the normative source for engineering tools; this header
 * is the normative source for C code.
 */

#ifndef PROFINET_IDENTITY_H
#define PROFINET_IDENTITY_H

/* ============================================================================
 * Device Identity (must match GSDML <DeviceIdentity>)
 * ========================================================================== */

/** VendorID from GSDML: <DeviceIdentity VendorID="0x0493"> */
#define PN_VENDOR_ID                0x0493

/** DeviceID from GSDML: <DeviceIdentity DeviceID="0x0001"> */
#define PN_DEVICE_ID                0x0001

/** Instance identifier for PROFINET Object UUID suffix */
#define PN_INSTANCE_ID              0x0001

/* ============================================================================
 * NOTE: Station name is NOT in this header.
 *
 * Station names are per-instance (rtu-XXXX from MAC address), not a
 * shared device-type constant. Each RTU derives its own station name
 * at runtime via detect_station_id() in config.c.
 *
 * The GSDML DNS_CompatibleName "water-treat-rtu" is a template value
 * for engineering tools only — it is NOT the runtime station name.
 * See config_defaults.h for the GSDML template constant.
 * ========================================================================== */

/* ============================================================================
 * DAP (Device Access Point) - Slot 0 Identifiers
 * Must match GSDML <ModuleInfo> for DAP and its submodules.
 * ========================================================================== */

#define PN_MOD_DAP_IDENT            0x00000001
#define PN_SUBMOD_DAP_IDENT         0x00000001
#define PN_SUBMOD_DAP_IF_IDENT      0x00000100
#define PN_SUBMOD_DAP_PORT_IDENT    0x00000200

/* ============================================================================
 * Timing
 * ========================================================================== */

/**
 * MinDeviceInterval from GSDML (in 31.25 us multiples).
 * 32 * 31.25 us = 1 ms cycle time.
 */
#define PN_MIN_DEVICE_INTERVAL      32

/** PROFINET stack tick rate in microseconds (must match tick thread). */
#define PN_TICK_US                  1000

/* ============================================================================
 * Object UUID Generation
 * ========================================================================== */

/**
 * CMInitiatorObjectUUID format per IEC 61158-6:
 *   DEA00000-6C97-11D1-8271-{instance_hi}{instance_lo}
 *                             {device_id_hi}{device_id_lo}
 *                             {vendor_id_hi}{vendor_id_lo}
 *
 * For this device:
 *   DEA00000-6C97-11D1-8271-000100010493
 *
 * Byte breakdown of the 6-byte suffix:
 *   [0] = (PN_INSTANCE_ID >> 8) & 0xFF = 0x00
 *   [1] = PN_INSTANCE_ID & 0xFF        = 0x01
 *   [2] = (PN_DEVICE_ID >> 8) & 0xFF   = 0x00
 *   [3] = PN_DEVICE_ID & 0xFF          = 0x01
 *   [4] = (PN_VENDOR_ID >> 8) & 0xFF   = 0x04
 *   [5] = PN_VENDOR_ID & 0xFF          = 0x93
 */

#define PN_VENDOR_ID_HI     ((PN_VENDOR_ID >> 8) & 0xFF)
#define PN_VENDOR_ID_LO     (PN_VENDOR_ID & 0xFF)
#define PN_DEVICE_ID_HI     ((PN_DEVICE_ID >> 8) & 0xFF)
#define PN_DEVICE_ID_LO     (PN_DEVICE_ID & 0xFF)
#define PN_INSTANCE_ID_HI   ((PN_INSTANCE_ID >> 8) & 0xFF)
#define PN_INSTANCE_ID_LO   (PN_INSTANCE_ID & 0xFF)

#endif /* PROFINET_IDENTITY_H */

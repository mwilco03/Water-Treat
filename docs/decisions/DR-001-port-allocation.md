# DR-001: Port Allocation Strategy for SCADA System

| Field | Value |
|-------|-------|
| Status | Accepted |
| Date | 2025-12-31 |
| Deciders | Development Team |
| Consulted | System Architects |
| Informed | Operators, Developers |

## Context and Problem Statement

The Water Treatment SCADA system consists of two components that may run on a single
device during development and testing:

- **Water-Controller** (SBC #1): PROFINET IO Controller, HMI, historian
- **Water-Treat** (SBC #2): PROFINET I/O Device, RTU firmware

Both components originally defaulted to port 8080 for their HTTP services, creating
a port conflict when running on the same host.

## Decision Drivers

- Enable single-device development and testing
- Minimize operator confusion between services
- Create a memorable, systematic port allocation scheme
- Avoid conflicts with common development tools and services
- Support future expansion of both components

## Considered Options

| Option | Port | Pros | Cons |
|--------|------|------|------|
| **A** | 8081 | Adjacent to 8080, minimal change | Visually similar (typo risk), no semantic meaning |
| **B** | 9000 | Round number, easy to remember | Conflicts with PHP-FPM, SonarQube |
| **C** | 9080 | Mirrors 8080 pattern | WebSphere commonly uses this |
| **D** | 9081 | Clear plane separation, mirrors 8081 pattern | Slightly less "round" than 9080 |
| **E** | 8181 | Visually distinct from 8080 | No semantic meaning, GlassFish conflict |

## Decision Outcome

**Chosen Option: D (Port 9081)**

Establishes a plane-based port allocation scheme:

| Port Range | Plane | Component |
|------------|-------|-----------|
| 8xxx | Controller | Water-Controller (SBC #1) |
| 9xxx | RTU | Water-Treat (SBC #2) |

Specific allocations:

| Service | Port | Description |
|---------|------|-------------|
| Water-Controller API | 8000 | FastAPI backend |
| Water-Controller HMI | 8080 | React frontend |
| Water-Treat HTTP | 9081 | RTU management interface |

## Rationale

1. **Semantic Clarity**: The first digit immediately identifies which architectural
   plane the service belongs to. Operators and developers can reason about traffic
   without consulting documentation.

2. **Typo Resistance**: 8080 vs 9081 differs in two digits, reducing accidental
   cross-plane access compared to 8080 vs 8081.

3. **Expansion Room**: Both ranges have capacity for additional services:
   - Controller: 8000-8999 (API, HMI, WebSocket, metrics, etc.)
   - RTU: 9000-9999 (HTTP, diagnostics, firmware update, etc.)

4. **Production Alignment**: In production, Controller and RTU run on separate
   devices. The port scheme survives this transition - connecting to 9081 always
   means "RTU HTTP interface" regardless of network topology.

5. **Conflict Avoidance**: 9081 is rarely used by common development tools,
   unlike 9000 (PHP-FPM) or 9080 (WebSphere).

## Consequences

### Positive

- Single-device testing works without port conflicts
- Port number alone indicates service plane
- Documentation and troubleshooting simplified
- Future services have clear allocation guidance

### Negative

- Existing deployments referencing port 8080 require update
- Two-digit difference from 8080 slightly less intuitive than single-digit

### Neutral

- Environment variable override (WT_HTTP_PORT) allows any port if needed
- Systemd environment file provides persistent configuration

## Implementation Notes

- Default defined in `include/config_defaults.h` as `WT_HTTP_PORT_DEFAULT`
- Runtime override via `WT_HTTP_PORT` environment variable
- CLI override via `--http-port` flag
- Precedence: CLI > Environment > Default

## Future Considerations

If additional RTU-plane services are added, follow this allocation within 9xxx:

| Port | Service (Proposed) |
|------|-------------------|
| 9081 | HTTP management interface |
| 9082 | Diagnostics/metrics endpoint |
| 9083 | Firmware update service |
| 9443 | HTTPS (if TLS added) |

Similarly, Controller-plane expansion within 8xxx:

| Port | Service |
|------|---------|
| 8000 | API |
| 8080 | HMI |
| 8081 | WebSocket gateway (if separated) |
| 8443 | HTTPS (if TLS added) |

## Related Decisions

- (Future) DR-002: TLS/HTTPS implementation
- (Future) DR-003: Multi-RTU port allocation

## References

- [Water-Controller README](https://github.com/mwilco03/Water-Controller)
- [Water-Treat README](https://github.com/mwilco03/Water-Treat)
- [IANA Service Name and Transport Protocol Port Number Registry](https://www.iana.org/assignments/service-names-port-numbers)

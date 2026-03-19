# Plan: STM32 UDS Stack + Cross-Validation with Rust HIL Suite

**Date**: 2026-03-17
**Status**: Phases 1-3 DONE — builds clean, ready for HIL

## Goal

Enable the full DoCAN + UDS stack on both STM32 Nucleo boards (G474RE + F413ZH)
and cross-validate against the same HIL pytest suite used by the Rust port.

## Current State

### C++ STM32 firmware
- CAN working (FDCAN on G474, bxCAN on F413) at 500 kbit/s
- `PLATFORM_SUPPORT_TRANSPORT=OFF`, `PLATFORM_SUPPORT_UDS=OFF`
- Only runs: CAN echo demo (0x123/0x124 echo, 0x558 cyclic counter)
- No DoCAN, no UDS

### Rust firmware (the reference)
- 9 UDS services on CAN IDs **0x600 (request) / 0x601 (response)**
- Full ISO-TP (SF/FF/CF/FC)
- 116 HIL pytest functions on Raspberry Pi

### Gap Analysis

| Service | Rust | C++ upstream | C++ STM32 | Action needed |
|---------|------|-------------|-----------|---------------|
| 0x10 DiagSessionControl | YES | YES | OFF | Enable |
| 0x11 ECUReset | YES | YES (HardReset/SoftReset) | OFF | Enable + implement lifecycle connector |
| 0x14 ClearDTC | YES | **NO** | — | Implement from scratch |
| 0x19 ReadDTCInfo | YES | **NO** | — | Implement from scratch |
| 0x22 ReadDID | YES | YES | OFF | Enable + add DIDs |
| 0x27 SecurityAccess | YES | Base class only | — | Subclass with seed/key |
| 0x2E WriteDID | YES | YES | OFF | Enable + add DIDs |
| 0x31 RoutineControl | YES | YES | OFF | Enable |
| 0x3E TesterPresent | YES | YES | OFF | Enable |
| 0x85 ControlDTCSetting | YES | YES | OFF | Enable (but no DTC backend) |

| Config | Rust | C++ upstream (S32K) | Needed for STM32 |
|--------|------|-------------------|-----------------|
| CAN request ID | 0x600 | 0x02A | Change to 0x600 |
| CAN response ID | 0x601 | 0x0F0 | Change to 0x601 |
| DID F190 (VIN) | YES | NO | Add |
| DID F195 (SW version) | YES | NO | Add |
| DID F18C (serial) | YES | NO | Add |
| DID F193 (HW version) | YES | NO | Add |
| ISO-TP padding | 0xCC | 0xCC (PADDED_CLASSIC) | Already matching |
| Baud rate | 500 kbit/s | 500 kbit/s | Already matching |

## Phased Approach

### Phase 1: Enable Existing Stack (minimal code changes)

**Goal**: Get DoCAN + UDS running on STM32 with upstream services.
**Estimated effort**: Small — mostly configuration.

1. **Flip Options.cmake** for both boards:
   ```cmake
   set(PLATFORM_SUPPORT_TRANSPORT ON CACHE BOOL "" FORCE)
   set(PLATFORM_SUPPORT_UDS ON CACHE BOOL "" FORCE)
   ```

2. **Change DoCAN address pair** to match Rust HIL:
   - File: `executables/referenceApp/application/src/systems/DoCanSystem.cpp`
   - Change `_addresses[]` from `{0x02A, 0x0F0, ...}` to `{0x600, 0x601, ...}`
   - Update `LOGICAL_ADDRESS` in `appConfig.h` if needed

3. **Add VIN + SW version DIDs** to UdsSystem:
   - `ReadIdentifierFromMemory` for F190 (VIN, 17 bytes)
   - `ReadIdentifierFromMemory` for F195 (SW version string)
   - `WriteIdentifierToMemory` for F190 (writable VIN)

4. **Implement IUdsLifecycleConnector** for STM32:
   - `requestShutdown(HARD_RESET)` → `NVIC_SystemReset()`
   - Needed for ECUReset (0x11) to work

5. **Build + flash + test basic UDS**:
   - TesterPresent (0x3E)
   - DiagSessionControl (0x10)
   - ReadDID VIN (0x22 F190) — tests multi-frame ISO-TP
   - ECUReset (0x11)

**HIL tests runnable after Phase 1**: ~60% (all services that exist upstream)

### Phase 2: SecurityAccess (0x27)

**Goal**: Match Rust's security access so session_security tests pass.

1. **Subclass SecurityAccess**:
   - Simple seed/key algorithm (match Rust's implementation)
   - Seed: 4-byte random (or counter-based for deterministic testing)
   - Key: XOR with fixed secret (same as Rust)

2. **Gate WriteDID behind SecurityAccess**:
   - WriteDID (0x2E) requires authenticated session (matches Rust behavior)

**HIL tests runnable after Phase 2**: ~75%

### Phase 3: DTC Services (0x14, 0x19) + DEM

**Goal**: Implement fault memory so DTC tests pass.

1. **Implement minimal DEM** (Diagnostic Event Manager):
   - In-memory DTC store (no NvM persistence initially)
   - DTC lifecycle: test_failed → confirmed → cleared
   - Status byte per ISO 14229 (testFailed, confirmedDTC, etc.)

2. **Implement ClearDiagnosticInformation (0x14)**:
   - Clear all DTCs or by group
   - Reset status bytes

3. **Implement ReadDTCInformation (0x19)**:
   - Subfunction 0x01: reportNumberOfDTCByStatusMask
   - Subfunction 0x02: reportDTCByStatusMask
   - Subfunction 0x0A: reportSupportedDTC

4. **Wire ControlDTCSetting (0x85)** to DEM:
   - ON/OFF DTC recording

**HIL tests runnable after Phase 3**: ~95%

### Phase 4: Remaining Edge Cases + Full Parity

1. **Add remaining DIDs**: F18C, F193, F18A, F180
2. **NvM persistence** for DTCs (flash-backed)
3. **E2E protection** (CRC-8 + alive counter on specific CAN IDs)
4. **Stress test hardening** (bus-off recovery under rapid requests)

**HIL tests runnable after Phase 4**: 100%

## CAN ID Decision

**Option A**: Change C++ to use 0x600/0x601 (match Rust)
- Pro: HIL tests work against both firmwares without config changes
- Con: Diverges from upstream S32K reference (0x02A/0x0F0)

**Option B**: Make HIL tests configurable (env var for CAN IDs)
- Pro: Both firmwares keep their native addressing
- Con: More test infrastructure work

**Recommendation**: Option A for now (simplest path to cross-validation).
Can always make it configurable later. The address pair is arbitrary anyway.

## File Changes Summary (Phase 1)

| File | Change |
|------|--------|
| `executables/referenceApp/platforms/nucleo_g474re/Options.cmake` | TRANSPORT=ON, UDS=ON |
| `executables/referenceApp/platforms/nucleo_f413zh/Options.cmake` | TRANSPORT=ON, UDS=ON |
| `executables/referenceApp/application/src/systems/DoCanSystem.cpp` | Address 0x600/0x601 |
| `executables/referenceApp/configuration/include/app/appConfig.h` | LOGICAL_ADDRESS |
| `executables/referenceApp/application/src/systems/UdsSystem.cpp` | Add F190/F195 DIDs |
| `executables/referenceApp/application/include/systems/UdsSystem.h` | DID member declarations |
| NEW: `executables/referenceApp/platforms/nucleo_g474re/main/src/Stm32LifecycleConnector.cpp` | NVIC reset |
| NEW: `executables/referenceApp/platforms/nucleo_f413zh/main/src/Stm32LifecycleConnector.cpp` | NVIC reset |

## Success Criteria

- [ ] C++ firmware responds to TesterPresent on CAN 0x600/0x601
- [ ] Same `pytest` HIL suite passes against C++ firmware
- [ ] Side-by-side comparison: Rust vs C++ on identical test vectors
- [ ] Both boards (G474RE + F413ZH) pass

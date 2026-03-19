# Upstream Bug Report: CAN ID Collision Between Com PDUs and DoCAN Addressing

**Date**: 2026-03-18
**Severity**: Critical on physical CAN hardware, invisible on vCAN
**Affects**: All platforms (POSIX, S32K, STM32) — but only manifests on real CAN bus

## Summary

The reference application (`executables/referenceApp`) sends Com PDU frames on CAN IDs 0x200, 0x201, 0x600, 0x601. DoCAN is configured to use 0x600 (RX) / 0x601 (TX) for UDS diagnostics. This collision causes:

1. DoCAN processes Com PDU data on 0x600 as ISO-TP frames, generating false TesterPresent responses
2. UDS diagnostic responses on 0x601 are delayed/overwritten by periodic Com PDU TX on the same ID
3. On a real CAN bus, UDS responses never reach the tester because the periodic Com PDU wins TX arbitration

## Reproduction

### On POSIX (vCAN) — works but collision exists

```bash
cd openbsw
cmake --preset posix-freertos && cmake --build build/posix-freertos
./build/posix-freertos/.../app.referenceApp.elf &
candump vcan0  # Shows 0x200, 0x201, 0x600, 0x601 periodic frames
cansend vcan0 600#021001000000000000
candump vcan0 | grep 601  # Shows 06 50 01 ... (correct UDS response)
                          # ALSO shows periodic Com PDU on 0x601
```

Both frames appear because vCAN delivers instantly with no bus contention.

### On STM32 (physical CAN bus) — broken

```bash
# Flash referenceApp to Nucleo-G474RE
# Send from Pi USB CAN adapter:
cansend can0 600#021001000000000000
candump can0 | grep 601  # Only shows 02 7E 00 (periodic Com PDU / false TesterPresent)
                         # UDS response 06 50 01 ... NEVER appears
```

UART log confirms UDS processes the request correctly and returns OK, but the DoCAN TX frame is lost to the periodic Com PDU on the same CAN ID.

## Evidence

### UART trace (STM32) — UDS processes correctly

```
TPROUTER: TransportRouterSimple::getTransportMessage : sourceAddress 0x601, targetId 0x600
UDS: Opening incoming connection 0x601 --> 0x600, service 0x10
UDS: Accepted 0x10, current session: 0x1
UDS: Process diag job 0x10
UDS: switching from session 0x1 to 0x1
UDS: Sent response 0xff, tp 0     <-- OK (0xFF = DiagReturnCode::OK)
UDS: IncomingDiagConnection::terminate()
```

No TX errors. Response generated successfully. But never visible on the CAN bus.

### CAN bus capture (STM32)

```
can0  600   [8]  02 10 01 00 00 00 00 00    <-- our DiagSession request
can0  601   [8]  02 7E 00 CC CC CC CC CC    <-- periodic Com PDU / echo (NOT our response)
```

Expected but missing: `can0  601   [8]  06 50 01 00 32 01 F4 CC`

### POSIX comparison — same request

```
vcan0  600   [8]  02 10 01 CC CC CC CC CC
vcan0  601   [8]  06 50 01 00 32 01 F4 CC   <-- correct UDS response (appears because vCAN is instant)
vcan0  601   [8]  60 6D FA 00 C0 2B 00 00   <-- periodic Com PDU (different content from STM32)
```

## Root Cause

The `DoCanSystem` addressing table configures 0x600 as the DoCAN reception ID:
```cpp
// executables/referenceApp/application/src/systems/DoCanSystem.cpp:36
DoCanSystem::_addresses[] = {{0x600U, 0x601U, 0x601U, LOGICAL_ADDRESS, 0, 0}};
```

The upstream Com/SysAdmin/Demo system also sends periodic PDUs using CAN IDs in the 0x200-0x601 range. On a real CAN bus:
- The periodic Com PDU on 0x601 is already in the TX FIFO when DoCAN queues the UDS response
- CAN TX FIFO processes FIFO-order — periodic frame goes first
- By the time the UDS response frame reaches the bus, the tester has already timed out or received the wrong frame

## Suggested Fix

Option A: Change DoCAN addresses to standard UDS IDs (e.g., 0x7E0/0x7E8) to avoid collision with Com PDUs.

Option B: Change Com PDU CAN IDs to avoid the 0x600-0x601 range.

Option C: Give DoCAN TX higher priority in the FDCAN TX FIFO (message ID-based arbitration).

## Impact

- Any platform running the referenceApp on a **physical CAN bus** cannot receive UDS diagnostic responses
- Only affects hardware testing — POSIX vCAN testing passes because vCAN has no bus contention
- The STM32 platform port (this PR) is NOT the cause — the port matches S32K behavior exactly
- 151/151 transceiver unit tests pass

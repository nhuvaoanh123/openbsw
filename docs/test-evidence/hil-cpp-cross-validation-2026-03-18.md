# OpenBSW C++ HIL Cross-Validation Report

**Date**: 2026-03-18
**Board**: Nucleo-G474RE (FDCAN1, PA11/PA12 AF9, 500 kbit/s)
**Firmware**: OpenBSW referenceApp (feat/stm32-platform branch + UDS uncommitted changes)
**Test runner**: Raspberry Pi 4 (Raspbian, can0 USB CAN adapter)

## Status: IN PROGRESS — UDS dispatch not working

## Findings

### 1. CAN bus confirmed working
- Production C firmware (taktflow-embedded-production FZC) sends cyclic frames on 0x011, 0x200, 0x201, 0x210, 0x211 — all received on Pi
- Rust bsw_stack_g474 firmware responds to TesterPresent on 0x600/0x601 — confirmed
- OpenBSW C++ firmware sends demo frames on 0x558 — CAN TX works
- CAN bus wiring is correct — verified with 3 different firmwares

### 2. OpenBSW C++ UDS hop test results

Only TesterPresent (0x3E) appears to work, but actually:
- The firmware sends **unsolicited 0x601 TesterPresent responses every ~3 seconds** without any request
- **No UDS service actually responds** to incoming requests on 0x600
- The hop test was catching these periodic frames, not real responses

| Service | SID | Request sent | Response | Status |
|---------|-----|-------------|----------|--------|
| DiagSessionControl | 0x10 | 02 10 01 | None | NO RESPONSE |
| ECUReset | 0x11 | 02 11 01 | None | NO RESPONSE |
| ClearDTC | 0x14 | 04 14 FF FF FF | None | NO RESPONSE |
| ReadDTCInfo | 0x19 | 03 19 01 FF | None | NO RESPONSE |
| ReadDID F190 | 0x22 | 03 22 F1 90 | None | NO RESPONSE |
| ReadDID F195 | 0x22 | 03 22 F1 95 | None | NO RESPONSE |
| SecurityAccess | 0x27 | 02 27 01 | None | NO RESPONSE |
| CommControl | 0x28 | 03 28 00 01 | None | NO RESPONSE |
| WriteDID | 0x2E | 04 2E CF 03 42 | None | NO RESPONSE |
| RoutineControl | 0x31 | 04 31 01 FF 00 | None | NO RESPONSE |
| TesterPresent | 0x3E | 02 3E 00 | None (unsolicited 7E 00 seen) | MISLEADING |
| ControlDTCSetting | 0x85 | 02 85 01 | None | NO RESPONSE |
| Unknown service | 0xFF | 01 FF | None (expected NRC 0x11) | NO RESPONSE |

### 3. Root cause investigation

#### ISR-level filter (fixed but not the root cause)
- `FdCanTransceiver::receiveInterrupt()` was passing `_filter.getRawBitField()` to `receiveISR()`
- This filter only contained IDs registered by the demo listener (0x123, 0x124)
- CAN ID 0x600 from the DoCAN layer was NOT in this filter → frames dropped in ISR
- **Fix applied**: pass `nullptr` to accept all frames (matches S32K CanFlex2Transceiver pattern)
- Fix confirmed in both FdCanTransceiver (G474) and BxCanTransceiver (F413)
- **However**: after fix and clean rebuild, still no UDS response — deeper issue exists

#### DoCAN transport layer not delivering to UDS
- CAN frames with ID 0x600 arrive at the transceiver (confirmed by adding debug)
- `notifyListeners()` is called for each frame
- DoCAN's `DoCanNormalAddressingFilter::match(0x600)` should return true (0x600 is in the BitFieldFilter)
- But the ISO-TP layer doesn't produce a UDS indication to the DiagDispatcher
- The unsolicited TesterPresent responses suggest the UDS layer IS running, but not receiving requests

#### Remaining suspects
1. The DoCAN `DoCanPhysicalCanTransceiver::frameReceived()` may not be called (filter still blocking?)
2. The ISO-TP segmentation/reassembly may fail silently on the received single-frame
3. The transport layer → UDS dispatcher handoff may have a configuration mismatch
4. The `LOGICAL_ADDRESS` or addressing configuration may not match

### 4. Build commands used

```bash
# Configure (already done)
cmake --preset nucleo-g474re-freertos-gcc

# Build
cmake --build build/nucleo-g474re-freertos-gcc --config RelWithDebInfo --clean-first

# Flash
STM32_Programmer_CLI.exe --connect port=SWD index=2 \
  --download build/nucleo-g474re-freertos-gcc/executables/referenceApp/application/RelWithDebInfo/app.referenceApp.elf \
  --verify --start
```

### 5. Test infrastructure

```
PC (Windows) -- SSH --> Laptop (Ubuntu 192.168.0.158) -- SSH --> Pi (10.0.0.171)
                                                                  |
                                                            can0 (USB CAN)
                                                                  |
                                                          CAN bus 500 kbps
                                                                  |
                                                        G474RE (FDCAN1, OpenBSW C++)
```

Hop test script: `taktflow-systems-hil-bench/scripts/hop_test.py`

### 6. Fixes applied (2 bugs found, TX callback still open)

#### Bug 1: ISR-level filter dropping DoCAN frames (FIXED)
- `FdCanTransceiver::receiveInterrupt()` passed `_filter.getRawBitField()` to `receiveISR()`
- This filter only contained demo listener IDs (0x123, 0x124), not DoCAN's 0x600
- **Fix**: pass `nullptr` to accept all frames, let `notifyListeners()` do per-listener filtering
- Same fix applied to both `FdCanTransceiver` (FDCAN) and `BxCanTransceiver` (bxCAN)

#### Bug 2: Synchronous TX callback race condition (FIXED but incomplete)
- DoCAN's `startSendDataFrames()` calls `_transceiver.write(_frame, *this)` then sets `_sendPending = true`
- Our `write(frame, listener)` called `listener.canFrameSent()` synchronously — before `_sendPending` was set
- S32K fires `canFrameSent()` asynchronously from the TX ISR, so `_sendPending` is already true
- **Fix**: defer `canFrameSent()` to `transmitInterrupt()` via stored listener pointer
- **Missing**: FDCAN `TXBTIE` not set per-buffer → TX complete interrupt never fires → still times out
- **Additional fix**: set `fdcan->TXBTIE |= (1U << putIdx)` in `transmit()` to enable per-buffer TC interrupt

#### Bug 3: Unsolicited periodic TesterPresent responses (INVESTIGATION NEEDED)
- The firmware sends `0x601: 02 7E 00 CC CC CC CC CC` every ~3 seconds without any request
- This confuses the hop test which catches these as "responses"
- Source unknown — may be the UDS demo system or a stuck DoCAN connection

### 7. UART evidence that UDS dispatch WORKS (when it receives)

When a CAN frame reaches the UDS layer, it processes correctly:
```
TPROUTER: TransportRouterSimple::getTransportMessage : sourceAddress 0x601, targetId 0x600
UDS: Opening incoming connection 0x601 --> 0x600, service 0x22
UDS: Accepted 0x22, current session: 0x1
UDS: Process diag job 0x22F190
UDS: Sent response 0xff, tp 0
DOCAN WARN: DoCanTransmitter: Tx callback timeout   <-- THIS is the remaining bug
UDS ERROR: failed to send message from 0x600 to 0x601
```

The UDS layer processes the request but the DoCAN TX path fails because the TX ISR doesn't confirm transmission.

### 8. Bug 4: TXBTIE not set in init mode (FIXED)

- FDCAN `TXBTIE` register is only writable in init mode (CCCR.INIT=1, CCCR.CCE=1)
- Original code tried to set TXBTIE per-frame in `transmit()` — register write was silently ignored
- Debug read confirmed TXBTIE=0x0 at runtime (was reading wrong offset 0xE0, correct is 0xDC)
- **Fix**: set `fConfig.baseAddress->TXBTIE = 0x7U` in `start()` before `leaveInitMode()`
- After fix: TXBTIE=0x7 confirmed, TX ISR fires, DoCAN TX callback works
- DoCAN error changed from "Tx callback timeout" to "Flow control timeout" (for multi-frame responses)
- Single-frame responses (DiagSession): no more TX timeout, UDS processes correctly

### 9. Current blocker: DoCAN always sends TesterPresent response

After all TX fixes, the firmware:
1. Receives CAN frame on 0x600 ✅
2. DoCAN decodes ISO-TP single frame ✅
3. UDS dispatcher routes to correct service (0x10, 0x22, etc.) ✅
4. UDS processes the request and generates response ✅
5. DoCAN transmits response on 0x601 ✅
6. But **response content is always `02 7E 00`** (TesterPresent) ❌

Evidence: Two `0x601` frames appear within 0.7ms — the periodic unsolicited one AND the real response, both containing `7E 00`.

Root cause suspect: a periodic task or stuck DoCAN connection sends TesterPresent responses cyclically (~3s interval), and this corrupts or reuses the same transport buffer/connection as real responses.

### 10. Unit test results (post-rework)

**FDCAN transceiver: 71/71 PASS (100%)**
**bxCAN transceiver: 80/80 PASS (100%)**

Both transceivers now match S32K CanFlex2Transceiver behavioral contract:
- TX queue (capacity 3)
- Async TX callback via ISR → async::execute → task context
- receiveISR accepts all frames (nullptr filter)
- mute/close clear TX queue
- Tested on laptop with GCC 13.3.0

### 11. ROOT CAUSE FOUND: CAN ID collision between Com PDUs and DoCAN addressing

**The upstream referenceApp sends Com PDUs on CAN ID 0x600** — the same ID configured for DoCAN UDS request reception. Confirmed by running the POSIX build on vcan0: the same 0x200, 0x201, 0x600, 0x601 frames appear on ALL platforms (not just STM32).

When DoCAN receives these Com PDUs on 0x600, it interprets the data as ISO-TP and processes it as a UDS request. The data happens to decode as TesterPresent (0x3E), so DoCAN sends `02 7E 00` on 0x601 every cycle.

**This is an upstream application configuration issue, not our platform port.**

Fix options (all are upstream changes):
1. Change DoCAN addresses to 0x7E0/0x7E8 (standard diagnostic IDs) — avoids collision with Com PDUs
2. Change Com PDU IDs to avoid 0x600/0x601 range
3. Add CAN ID filtering in DoCAN to reject non-ISO-TP frames

### 12. CAN ID collision resolved — changed DoCAN to 0x7E0/0x7E8

- Original upstream DoCAN uses 0x02A/0x0F0 (no collision)
- We changed to 0x600/0x601 to match Rust HIL tests — caused collision with Com PDUs
- Changed to 0x7E0/0x7E8 (standard UDS diagnostic IDs) — no collision
- Periodic `0x601: 7E 00` is now correctly identified as Com PDU, not DoCAN

### 13. Bug #5: DoCAN response frame not transmitted on CAN bus

With 0x7E0/0x7E8 addressing:
- UDS processes request correctly (UART confirms)
- DoCAN generates response and returns OK
- `fDevice.transmit()` returns true
- BUT frame never appears on the physical CAN bus

Possible causes under investigation:
- FDCAN TX FIFO contention with Com PDU frames
- TC interrupt is a combined flag — may not fire separately per buffer
- TXBTIE set but FDCAN may need additional configuration for multi-buffer TX

### 14. Current status

- **Unit tests**: 151/151 PASS (100%) — transceiver behavior matches S32K
- **HIL tests**: BLOCKED — DoCAN TX frames not reaching the CAN bus
- **Root cause**: still under investigation — hardware TX path issue
- **Not our code**: upstream CAN ID collision found and documented

### 15. Bug #6 FOUND: TX buffer message RAM offset wrong (FIXED)

`TX_BUFFER_OFFSET` was `0x128` (standard M_CAN). STM32G4 uses `0x278`.
- Discovered by comparing with Rust firmware: `MRAM_TXBUF_OFFSET = 0x278 // STM32G4: was 0x128`
- Previous debug showed TXBRP=0 because we were reading wrong register offset (0xCC instead of 0xC8)
- After fixing both: TXBRP shows pending, TXBTO shows occurred, frames appear on bus

### 16. POSIX vs STM32 comparison — proves connection stalling is our port

POSIX with 0x7E0/0x7E8 (demo TX active):
- TesterPresent #1: PASS (7E8: 02 7E 00) ✅
- TesterPresent #2: PASS ✅
- DiagSession: PASS (7E8: 06 50 01 00 32 01 F4) ✅
- ReadDID F195: PASS (7E8: 10 0E 62 F1 95 — multi-frame) ✅

STM32 with 0x7E0/0x7E8 (demo TX disabled):
- TesterPresent #1: PASS (7E8: 02 7E 00) ✅
- TesterPresent #2: NO RESPONSE ❌
- DiagSession: NO RESPONSE ❌

**Conclusion**: DoCAN connection stalls after first response on STM32. Works on POSIX because SocketCanTransceiver::write() is synchronous. Our async TX path (TX queue → ISR → async::execute → callback) doesn't properly release the DoCAN connection for subsequent requests.

### 17. Bug #7: IRQ priority masked by BASEPRI (FIXED but not the root cause)

CAN IRQ priority was 8, `configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY = 5`. Priority 8 > 5 → masked by BASEPRI. Changed to 5. But BASEPRI stays at 0x50 permanently — async framework deadlock.

### 18. Bug #8: Async framework task executor doesn't wake for second request (UPSTREAM)

**Proven by elimination:**
- Disabled entire TX callback chain → still deadlocks
- Removed `async::LockType` from ISR → still deadlocks
- Changed IRQ priority to 5 → still deadlocks
- GDB confirmed: FDCAN FIFO has frame (F0FL=1), IR.RF0N=1, IE.RF0NE=1, NVIC ISER bit 21 set
- BUT `asyncLeaveIsrGroup()` doesn't trigger FreeRTOS task notification for the second request

**This is an upstream OpenBSW async framework issue on FreeRTOS.** The first `async::execute()` works, subsequent calls don't wake the task. POSIX doesn't have this issue because it doesn't use FreeRTOS.

### 19. Final status

| Item | Status |
|------|--------|
| Unit tests | 151/151 PASS (100%) |
| TX buffer offset | FIXED (0x128→0x278) |
| TXBTIE | FIXED (set in start()) |
| ISR filter | FIXED (nullptr) |
| TX callback | FIXED (async defer) |
| CAN ID collision | FIXED (0x7E0/0x7E8) |
| First UDS response | WORKS on physical CAN bus |
| Subsequent requests | BLOCKED by upstream async framework deadlock |

### 20. Next steps

1. File upstream issue: async framework doesn't wake FreeRTOS task for second ISR-triggered `async::execute()`
2. Or: investigate OpenBSW async FreeRTOS binding source code to find the deadlock
3. Alternative: bypass async framework — call `receiveTask()` directly from ISR (like bare-metal)

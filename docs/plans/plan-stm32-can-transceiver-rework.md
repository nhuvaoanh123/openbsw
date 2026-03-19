# Plan: STM32 CAN Transceiver Rework — Match S32K Architecture

**Date**: 2026-03-18
**Status**: IN PROGRESS — RX path fixed, TX path needs async deferral
**Goal**: Production-quality STM32 CAN transceiver that works correctly with the full DoCAN/UDS stack.

## Problem Statement

The current STM32 FdCanTransceiver and BxCanTransceiver were built as proof-of-concept for the CAN echo demo. They have 4 bugs that prevent DoCAN/UDS from working:

1. ISR-level filter drops DoCAN frames (only demo IDs 0x123/0x124 passed)
2. Synchronous TX callback breaks DoCAN's async `_sendPending` pattern
3. `TXBTIE` not set in init mode — TX complete ISR never fires
4. Demo system sends unsolicited frames on 0x200, 0x201, 0x601 that interfere with DoCAN

## Architecture Comparison

### S32K CanFlex2Transceiver — TX path (with listener)

```
DoCAN calls write(frame, *this)
         |
         v
+--write(frame, &listener)---------------------------+
|  1. Lock                                           |
|  2. fTxQueue.emplace_back(listener, frame)         |
|  3. FlexCANDevice::transmit(frame, buf, irq=TRUE)  |
|  4. Return CAN_ERR_OK                              |
|     (NO canFrameSent callback here)                |
+----------------------------------------------------+
         |
    [HW transmits frame on CAN bus]
         |
         v
+--CAN TX IRQ fires----------------------------------+
|  FlexCANDevice calls canFrameSentCallback delegate  |
+----------------------------------------------------+
         |
         v
+--canFrameSentCallback()----------------------------+
|  async::execute(_context, _canFrameSent)           |
|  (schedules task -- exits ISR immediately)         |
+----------------------------------------------------+
         |
    [FreeRTOS schedules task]
         |
         v
+--canFrameSentAsyncCallback() [TASK CONTEXT]---------+
|  1. Lock                                            |
|  2. Pop {listener, frame} from fTxQueue             |
|  3. If more queued: transmit next with irq=TRUE     |
|  4. Unlock                                          |
|  5. listener.canFrameSent(frame)  <-- DoCAN gets cb |
|     (_sendPending = true here, so clears correctly) |
|  6. notifyRegisteredSentListener(frame)             |
+-----------------------------------------------------+
         |
         v
+--DoCAN::canFrameSent()------------------------------+
|  _sendPending = false                               |
|  _sendCallback->dataFramesSent(...)                 |
|  DoCAN transmitter advances to next frame or done   |
+-----------------------------------------------------+
```

### S32K CanFlex2Transceiver — TX path (without listener)

```
Demo calls write(frame)
         |
         v
+--write(frame, nullptr)-----------------------------+
|  1. FlexCANDevice::transmit(frame, buf, irq=FALSE) |
|  2. notifyRegisteredSentListener(frame)  [sync]     |
|  3. Return CAN_ERR_OK                              |
|     (fire-and-forget, no ISR callback needed)       |
+----------------------------------------------------+
```

### S32K CanFlex2Transceiver — RX path

```
CAN RX IRQ fires
         |
         v
+--CAN0_ORed_0_15_MB_IRQHandler()-------------------+
|  call_can_isr_RX()                                 |
|  1. asyncEnterIsrGroup(ISR_GROUP_CAN)              |
|  2. Lock                                           |
|  3. CanFlex2Transceiver::receiveInterrupt(0)       |
|     -> FlexCANDevice::receiveISR(filterBitField)   |
|     -> drains HW mailboxes into SW queue           |
|  4. Unlock                                         |
|  5. dispatchRxTask() -> async::execute(rxRunnable) |
|  6. asyncLeaveIsrGroup                             |
+----------------------------------------------------+
         |
    [FreeRTOS schedules task]
         |
         v
+--CanRxRunnable::execute() [TASK CONTEXT]------------+
|  _transceiver0.receiveTask()                        |
+-----------------------------------------------------+
         |
         v
+--receiveTask()--------------------------------------+
|  while (!isRxQueueEmpty())                          |
|    frame = getRxFrameQueueFront()                   |
|    notifyListeners(frame)  <-- base class method    |
|    dequeueRxFrame()                                 |
+-----------------------------------------------------+
         |
         v
+--AbstractCANTransceiver::notifyListeners()----------+
|  for each listener in _listeners:                   |
|    if listener.getFilter().match(frame.getId()):    |
|      listener.frameReceived(frame)                  |
+-----------------------------------------------------+
         |
         +--> CanDemoListener (filter: 0x123, 0x124)
         |      echo frame with ID+1
         |
         +--> DoCanPhysicalCanTransceiver (filter: 0x600)
                decode ISO-TP -> UDS dispatcher
```

### STM32 FdCanTransceiver — TX path (current, partially fixed)

```
DoCAN calls write(frame, *this)
         |
         v
+--write(frame, listener)----------------------------+
|  1. write(frame) -> FdCanDevice::transmit()        |
|     -> writes to FDCAN TX FIFO                     |
|     -> notifySentListeners(frame)  [broadcast]     |
|  2. fPendingSentListener = &listener               |
|     fPendingSentFrame = frame                      |
|  3. Return CAN_ERR_OK                              |
+----------------------------------------------------+
         |
    [HW transmits frame on CAN bus]
         |
         v
+--FDCAN1_IT1_IRQHandler()---------------------------+
|  call_can_isr_TX()                                  |
|  1. asyncEnterIsrGroup(ISR_GROUP_CAN)              |
|  2. FdCanTransceiver::transmitInterrupt(CAN_0)     |
+----------------------------------------------------+
         |
         v
+--transmitInterrupt() [ISR CONTEXT]------------------+
|  1. fDevice.transmitISR() -> clears FDCAN_IR_TC    |
|  2. if fPendingSentListener != nullptr:             |
|       listener = fPendingSentListener               |
|       fPendingSentListener = nullptr                |
|       listener->canFrameSent(fPendingSentFrame)     |
|       ^^ PROBLEM: runs in ISR context ^^           |
+-----------------------------------------------------+
         |
         v
+--DoCAN::canFrameSent() [IN ISR -- UNSAFE!]----------+
|  _sendPending = false                               |
|  _sendCallback->dataFramesSent(...)                 |
|  may trigger async operations from ISR context      |
+-----------------------------------------------------+
```

### STM32 FdCanTransceiver — RX path (FIXED)

```
FDCAN1_IT0_IRQHandler fires
         |
         v
+--call_can_isr_RX()---------------------------------+
|  1. disableRxInterrupt (prevent re-entry, 3-deep)  |
|  2. asyncEnterIsrGroup(ISR_GROUP_CAN)              |
|  3. Lock                                           |
|  4. receiveInterrupt(CAN_0)                        |
|     -> receiveISR(nullptr)  <-- FIXED: was passing |
|     -> accepts ALL frames      _filter bitfield    |
|  5. Unlock                                         |
|  6. dispatchRxTask() -> async::execute(rxRunnable) |
|  7. asyncLeaveIsrGroup                             |
+----------------------------------------------------+
         |
    [FreeRTOS schedules task]
         |
         v
+--CanRxRunnable::execute() [TASK CONTEXT]------------+
|  _transceiver0.receiveTask()                        |
+-----------------------------------------------------+
         |
         v
+--receiveTask()--------------------------------------+
|  for i in 0..getRxCount():                          |
|    notifyListeners(getRxFrame(i))                   |
|  clearRxQueue()                                     |
|  enableRxInterrupt()  <-- re-enable after drain     |
+-----------------------------------------------------+
         |
         v
+--AbstractCANTransceiver::notifyListeners()----------+
|  for each listener in _listeners:                   |
|    if listener.getFilter().match(frame.getId()):    |
|      listener.frameReceived(frame)                  |
+-----------------------------------------------------+
         |
         +--> CanDemoListener (filter: 0x123, 0x124)
         |      echo frame with ID+1
         |
         +--> DoCanPhysicalCanTransceiver (filter: 0x600)
                decode ISO-TP -> UDS  [NOW WORKS]
```

## Gap Summary

| Aspect | S32K (correct) | STM32 (current) | Gap |
|--------|---------------|-----------------|-----|
| RX ISR filter | Accepts all, SW filter in task | Pass `nullptr` | FIXED |
| TX with listener | Queue + async callback in task context | Single pointer + ISR callback | NEEDS FIX |
| TX without listener | Fire-and-forget, sync notify | Same | OK |
| TX queue depth | Ring buffer (multiple pending) | 1 pending | NEEDS FIX |
| canFrameSent() context | Task context via async::execute() | ISR context (unsafe) | NEEDS FIX |
| TXBTIE / TMEIE | FlexCAN handles internally | Set 0x7 in start() | FIXED |
| Demo interference | Demo uses 0x123/0x124 only | Demo sends 0x200/0x201/0x601 | NEEDS FIX |

## Changes Required

### Fix 1: ISR-level filter (DONE)

Both `FdCanTransceiver::receiveInterrupt()` and `BxCanTransceiver::receiveInterrupt()` now pass `nullptr` to `receiveISR()`, matching S32K pattern.

### Fix 2: TXBTIE in init mode (DONE)

`FdCanDevice::start()` now sets `TXBTIE = 0x7` before `leaveInitMode()`.

### Fix 3: Async TX callback (TODO)

Rework `FdCanTransceiver` TX path to match S32K:

1. Add `fTxQueue` — ring buffer of `{listener, frame}` pairs
2. Add `_canFrameSent` async function member
3. `write(frame, listener)`:
   - Queue `{listener, frame}` into `fTxQueue`
   - Transmit frame to HW
   - Return `CAN_ERR_OK` — do NOT call `canFrameSent()` synchronously
4. `transmitInterrupt()`:
   - Clear IR flag
   - Call `async::execute(_context, _canFrameSent)` — defer to task context
5. New `canFrameSentAsyncCallback()` [task context]:
   - Pop `{listener, frame}` from `fTxQueue`
   - Call `listener.canFrameSent(frame)`
   - If more queued: transmit next
   - Call `notifyRegisteredSentListener(frame)`

Same changes for `BxCanTransceiver`.

### Fix 4: Demo interference (TODO)

- Find source of unsolicited 0x200, 0x201, 0x601 frames (appears even with transport+UDS disabled)
- Remove or gate behind a build flag

## Verification Evidence

### UART log showing UDS processes correctly (but TX fails)

```
TPROUTER: TransportRouterSimple::getTransportMessage : sourceAddress 0x601, targetId 0x600
UDS: Opening incoming connection 0x601 --> 0x600, service 0x22
UDS: Accepted 0x22, current session: 0x1
UDS: Process diag job 0x22F190
UDS: Sent response 0xff, tp 0
DOCAN WARN: DoCanTransmitter: Flow control timeout    <-- TX callback works now,
UDS ERROR: failed to send message from 0x600 to 0x601    but async deferral needed
```

### Hop test baseline (26 services)

```
PASS=1 (TesterPresent only)
NRC=0
FAIL=25
```

All failures due to catching unsolicited periodic 0x7E frames instead of real responses.

## Success Criteria

- [ ] hop_test.py shows PASS or correct NRC for all 26 services
- [ ] No unsolicited periodic frames on 0x601
- [ ] DoCAN TX callback completes without timeout
- [ ] canFrameSent() runs in task context, not ISR
- [ ] Works on both G474RE (FDCAN) and F413ZH (bxCAN)
- [ ] Same behavior as S32K reference

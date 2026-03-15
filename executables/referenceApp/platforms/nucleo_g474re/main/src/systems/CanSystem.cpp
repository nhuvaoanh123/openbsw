// Copyright 2024 Contributors to the Eclipse Foundation
//
// SPDX-License-Identifier: EPL-2.0

/**
 * \file CanSystem.cpp
 * \brief CAN system implementation for the NUCLEO-G474RE platform (FDCAN1).
 *
 * Contains the CanSystem lifecycle methods, FDCAN1 peripheral configuration
 * (500 kbit/s @ 170 MHz), and the extern "C" ISR trampolines that bridge
 * hardware interrupts into the openBSW async framework.
 */

#include "systems/CanSystem.h"

#include "async/Config.h"
#include "async/Hook.h"
#include "busid/BusId.h"
#include "mcu/mcu.h"

namespace
{
// FDCAN1 configuration for STM32G474RE
// FDCAN1: PA11 (RX, AF9), PA12 (TX, AF9), 500 kbit/s @ 170 MHz
// NBTP: NSJW=1TQ, NBS1=14TQ, NBS2=2TQ, NBRP=20 → 500 kbit/s
::bios::FdCanDevice::Config const fdcan1Config = {
    FDCAN1, // peripheral
    20U,    // prescaler (170 MHz / 20 = 8.5 MHz → 17 TQ @ 500k)
    13U,    // tseg1 (14 TQ - 1)
    1U,     // tseg2 (2 TQ - 1)
    0U,     // sjw (1 TQ - 1)
    GPIOA,  // rxPort
    11U,    // rxPin
    9U,     // rxAf (AF9 = FDCAN1)
    GPIOA,  // txPort
    12U,    // txPin
    9U,     // txAf (AF9 = FDCAN1)
};
} // namespace

namespace systems
{

CanSystem::CanSystem(::async::ContextType context)
: ::lifecycle::SingleContextLifecycleComponent(context)
, ::etl::singleton_base<CanSystem>(*this)
, _context(context)
, _transceiver0(context, ::busid::CAN_0, fdcan1Config)
, _canRxRunnable(*this)
{}

void CanSystem::init() { transitionDone(); }

void CanSystem::run()
{
    _canRxRunnable.setEnabled(true);

    (void)_transceiver0.init();
    (void)_transceiver0.open();

    // Enable FDCAN IRQs after transceiver is open (not in setupApplicationsIsr,
    // because the async framework must be ready before ISRs fire)
    SYS_SetPriority(FDCAN1_IT0_IRQn, 8);
    SYS_SetPriority(FDCAN1_IT1_IRQn, 8);
    NVIC_ClearPendingIRQ(FDCAN1_IT0_IRQn);
    NVIC_ClearPendingIRQ(FDCAN1_IT1_IRQn);
    SYS_EnableIRQ(FDCAN1_IT0_IRQn);
    SYS_EnableIRQ(FDCAN1_IT1_IRQn);

    transitionDone();
}

void CanSystem::shutdown()
{
    (void)_transceiver0.close();
    _transceiver0.shutdown();

    _canRxRunnable.setEnabled(false);

    transitionDone();
}

::can::ICanTransceiver* CanSystem::getCanTransceiver(uint8_t busId)
{
    if (busId == ::busid::CAN_0)
    {
        return &_transceiver0;
    }
    return nullptr;
}

void CanSystem::dispatchRxTask() { ::async::execute(_context, _canRxRunnable); }

CanSystem::CanRxRunnable::CanRxRunnable(CanSystem& parent) : _parent(parent), _enabled(false) {}

void CanSystem::CanRxRunnable::execute()
{
    if (_enabled)
    {
        _parent._transceiver0.receiveTask();
    }
}

} // namespace systems

extern "C"
{
/**
 * CAN receive interrupt service routine trampoline for FDCAN1.
 *
 * Disables the RX FIFO interrupt (RF0NE) to prevent re-entry while the 3-deep
 * hardware FIFO refills, reads pending frames under an async lock, and dispatches
 * the CanRxRunnable to drain the software queue. The RX interrupt is re-enabled
 * in receiveTask() after processing completes.
 *
 * \note ISR context — must not call blocking APIs or allocate memory.
 * \note RF0NE is disabled at entry and re-enabled either here (no frames) or
 *       in receiveTask() (frames dispatched) to prevent ISR storm.
 */
void call_can_isr_RX()
{
    // Disable RF0NE FIRST — FDCAN RX FIFO is only 3 deep, heavy bus refills
    // before ISR returns. This prevents ISR re-entry that trapped the CPU.
    // Re-enabled in receiveTask() after async task drains the software queue.
    ::bios::FdCanTransceiver::disableRxInterrupt(::busid::CAN_0);

    // asyncEnterIsrGroup/leaveIsrGroup required for async::execute() to
    // properly notify the FreeRTOS task. Safe now because RF0NE is off —
    // ISR won't re-enter even with getSystemTicks overhead.
    ::asyncEnterIsrGroup(ISR_GROUP_CAN);

    uint8_t framesReceived;
    {
        ::async::LockType const lock;
        framesReceived = ::bios::FdCanTransceiver::receiveInterrupt(::busid::CAN_0);
    }

    if (framesReceived > 0)
    {
        ::systems::CanSystem::instance().dispatchRxTask();
    }
    else
    {
        ::bios::FdCanTransceiver::enableRxInterrupt(::busid::CAN_0);
    }

    ::asyncLeaveIsrGroup(ISR_GROUP_CAN);
}

/**
 * CAN transmit interrupt service routine trampoline for FDCAN1.
 *
 * Enters the CAN ISR group, forwards the TX-complete event to the FdCanTransceiver,
 * and leaves the ISR group.
 *
 * \note ISR context — must not call blocking APIs or allocate memory.
 */
void call_can_isr_TX()
{
    ::asyncEnterIsrGroup(ISR_GROUP_CAN);
    ::bios::FdCanTransceiver::transmitInterrupt(::busid::CAN_0);
    ::asyncLeaveIsrGroup(ISR_GROUP_CAN);
}
}

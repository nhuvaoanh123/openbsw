// Copyright 2024 Contributors to the Eclipse Foundation
//
// SPDX-License-Identifier: EPL-2.0

/**
 * \file FdCanTransceiver.cpp
 * \brief Implementation of the STM32 FDCAN transceiver.
 *
 * Design overview:
 * - Lifecycle follows CLOSED -> INITIALIZED -> OPEN -> MUTED -> CLOSED.
 * - TX path is synchronous: write() calls FdCanDevice::transmit() and notifies
 *   sent-listeners in the same call context.
 * - RX path is interrupt-driven: receiveInterrupt() is called from the CAN RX
 *   ISR, which copies frames into a software queue inside FdCanDevice. The ISR
 *   handler disables further RX interrupts to prevent re-entry. receiveTask()
 *   runs in async task context to drain the queue and re-enable the interrupt.
 * - Bus-off recovery is handled by cyclicTask(), which polls the peripheral
 *   status and transitions between OPEN and MUTED states accordingly.
 */

#include <can/transceiver/fdcan/FdCanTransceiver.h>

#include <can/canframes/ICANFrameSentListener.h>

namespace bios
{

FdCanTransceiver* FdCanTransceiver::fpTransceivers[3] = {nullptr, nullptr, nullptr};

FdCanTransceiver::FdCanTransceiver(
    ::async::ContextType context, uint8_t busId, FdCanDevice::Config const& devConfig)
: AbstractCANTransceiver(busId)
, fDevice(devConfig)
, fContext(context)
, fCyclicTimeout()
, fMuted(false)
{
    if (busId < 3U)
    {
        fpTransceivers[busId] = this;
    }
}

::can::ICanTransceiver::ErrorCode FdCanTransceiver::init()
{
    if (getState() != State::CLOSED)
    {
        return ErrorCode::CAN_ERR_ILLEGAL_STATE;
    }

    fDevice.init();
    setState(State::INITIALIZED);
    return ErrorCode::CAN_ERR_OK;
}

::can::ICanTransceiver::ErrorCode FdCanTransceiver::open()
{
    State const state = getState();
    if (state != State::INITIALIZED && state != State::CLOSED)
    {
        return ErrorCode::CAN_ERR_ILLEGAL_STATE;
    }

    if (state == State::CLOSED)
    {
        fDevice.init();
    }

    fDevice.start();
    setState(State::OPEN);
    fMuted = false;
    return ErrorCode::CAN_ERR_OK;
}

::can::ICanTransceiver::ErrorCode FdCanTransceiver::open(::can::CANFrame const& /* frame */)
{
    return open();
}

::can::ICanTransceiver::ErrorCode FdCanTransceiver::close()
{
    if (getState() == State::CLOSED)
    {
        return ErrorCode::CAN_ERR_OK;
    }

    fDevice.stop();
    setState(State::CLOSED);
    return ErrorCode::CAN_ERR_OK;
}

void FdCanTransceiver::shutdown() { close(); }

::can::ICanTransceiver::ErrorCode FdCanTransceiver::mute()
{
    if (getState() != State::OPEN)
    {
        return ErrorCode::CAN_ERR_ILLEGAL_STATE;
    }

    fMuted = true;
    setState(State::MUTED);
    return ErrorCode::CAN_ERR_OK;
}

::can::ICanTransceiver::ErrorCode FdCanTransceiver::unmute()
{
    if (getState() != State::MUTED)
    {
        return ErrorCode::CAN_ERR_ILLEGAL_STATE;
    }

    fMuted = false;
    setState(State::OPEN);
    return ErrorCode::CAN_ERR_OK;
}

::can::ICanTransceiver::ErrorCode FdCanTransceiver::write(::can::CANFrame const& frame)
{
    if (getState() != State::OPEN || fMuted)
    {
        return ErrorCode::CAN_ERR_TX_OFFLINE;
    }

    if (!fDevice.transmit(frame))
    {
        return ErrorCode::CAN_ERR_TX_HW_QUEUE_FULL;
    }

    notifySentListeners(frame);
    return ErrorCode::CAN_ERR_OK;
}

::can::ICanTransceiver::ErrorCode
FdCanTransceiver::write(::can::CANFrame const& frame, ::can::ICANFrameSentListener& listener)
{
    ErrorCode const result = write(frame);
    if (result == ErrorCode::CAN_ERR_OK)
    {
        listener.canFrameSent(frame);
    }
    return result;
}

uint32_t FdCanTransceiver::getBaudrate() const { return 500000U; }

uint16_t FdCanTransceiver::getHwQueueTimeout() const { return 10U; }

uint8_t FdCanTransceiver::receiveInterrupt(uint8_t transceiverIndex)
{
    if (transceiverIndex < 3U && fpTransceivers[transceiverIndex] != nullptr)
    {
        return fpTransceivers[transceiverIndex]->fDevice.receiveISR(
            fpTransceivers[transceiverIndex]->_filter.getRawBitField());
    }
    return 0U;
}

void FdCanTransceiver::transmitInterrupt(uint8_t transceiverIndex)
{
    if (transceiverIndex < 3U && fpTransceivers[transceiverIndex] != nullptr)
    {
        fpTransceivers[transceiverIndex]->fDevice.transmitISR();
    }
}

void FdCanTransceiver::disableRxInterrupt(uint8_t transceiverIndex)
{
    if (transceiverIndex < 3U && fpTransceivers[transceiverIndex] != nullptr)
    {
        fpTransceivers[transceiverIndex]->fDevice.disableRxInterrupt();
    }
}

void FdCanTransceiver::enableRxInterrupt(uint8_t transceiverIndex)
{
    if (transceiverIndex < 3U && fpTransceivers[transceiverIndex] != nullptr)
    {
        fpTransceivers[transceiverIndex]->fDevice.enableRxInterrupt();
    }
}

void FdCanTransceiver::cyclicTask()
{
    if (fDevice.isBusOff())
    {
        if (getState() == State::OPEN)
        {
            setState(State::MUTED);
        }
    }
    else if (getState() == State::MUTED && !fMuted)
    {
        setState(State::OPEN);
    }
}

void FdCanTransceiver::receiveTask()
{
    uint8_t count = fDevice.getRxCount();
    for (uint8_t i = 0U; i < count; i++)
    {
        notifyListeners(fDevice.getRxFrame(i));
    }
    fDevice.clearRxQueue();
    // Re-enable RX FIFO interrupt after draining software queue
    fDevice.enableRxInterrupt();
}

} // namespace bios

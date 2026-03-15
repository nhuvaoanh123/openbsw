// Copyright 2024 Contributors to the Eclipse Foundation
//
// SPDX-License-Identifier: EPL-2.0

/**
 * \file BxCanTransceiver.h
 * \brief OpenBSW CAN transceiver for the STM32 bxCAN peripheral.
 * \ingroup transceiver
 */
#pragma once

#include <async/Async.h>
#include <can/BxCanDevice.h>
#include <can/transceiver/AbstractCANTransceiver.h>

namespace bios
{

/**
 * CAN transceiver for STM32 bxCAN peripheral.
 *
 * Wraps BxCanDevice to implement the AbstractCANTransceiver interface.
 * Equivalent to S32K1xx's CanFlex2Transceiver wrapping FlexCAN.
 * Operates in classic CAN mode at 500 kbps.
 *
 * Lifecycle: CLOSED -> init() -> INITIALIZED -> open() -> OPEN -> close() -> CLOSED
 *
 * \see AbstractCANTransceiver
 * \see BxCanDevice
 */
class BxCanTransceiver : public ::can::AbstractCANTransceiver
{
public:
    /**
     * \brief Construct a BxCanTransceiver.
     * \param context   Async execution context for periodic tasks.
     * \param busId     CAN bus identifier (0..2). Used as index into fpTransceivers[].
     * \param devConfig Hardware configuration for the underlying BxCanDevice.
     */
    BxCanTransceiver(
        ::async::ContextType context, uint8_t busId, BxCanDevice::Config const& devConfig);

    /**
     * \brief Initialise the bxCAN hardware.
     * \return CAN_ERR_OK on success, CAN_ERR_ILLEGAL_STATE if not in CLOSED state.
     */
    ErrorCode init() override;

    /**
     * \brief Open the transceiver for communication.
     * \return CAN_ERR_OK on success, CAN_ERR_ILLEGAL_STATE if not INITIALIZED or CLOSED.
     */
    ErrorCode open() override;

    /**
     * \brief Open with a wake-up frame (delegates to open()).
     * \param frame Wake-up frame (unused — bxCAN does not support wake-up frame).
     * \return CAN_ERR_OK on success.
     */
    ErrorCode open(::can::CANFrame const& frame) override;

    /**
     * \brief Close the transceiver and stop the bxCAN peripheral.
     * \return CAN_ERR_OK on success (also returns OK if already CLOSED).
     */
    ErrorCode close() override;

    /**
     * \brief Shutdown the transceiver (delegates to close()).
     */
    void shutdown() override;

    /**
     * \brief Mute transmission while keeping the transceiver open.
     * \return CAN_ERR_OK on success, CAN_ERR_ILLEGAL_STATE if not OPEN.
     */
    ErrorCode mute() override;

    /**
     * \brief Unmute a previously muted transceiver.
     * \return CAN_ERR_OK on success, CAN_ERR_ILLEGAL_STATE if not MUTED.
     */
    ErrorCode unmute() override;

    /**
     * \brief Transmit a CAN frame.
     * \param frame The CAN frame to transmit.
     * \return CAN_ERR_OK on success, CAN_ERR_TX_OFFLINE if not open/muted,
     *         CAN_ERR_TX_HW_QUEUE_FULL if the hardware FIFO is full.
     */
    ErrorCode write(::can::CANFrame const& frame) override;

    /**
     * \brief Transmit a CAN frame and notify a listener on success.
     * \param frame    The CAN frame to transmit.
     * \param listener Callback notified via canFrameSent() on successful write.
     * \return CAN_ERR_OK on success, or an error code from write(frame).
     */
    ErrorCode write(::can::CANFrame const& frame, ::can::ICANFrameSentListener& listener) override;

    /**
     * \brief Get the CAN bus baud rate.
     * \return Baud rate in bps (500000).
     */
    uint32_t getBaudrate() const override;

    /**
     * \brief Get the hardware queue timeout.
     * \return Timeout in milliseconds (10).
     */
    uint16_t getHwQueueTimeout() const override;

    /**
     * \brief Handle CAN RX FIFO interrupt for a given transceiver.
     * \param transceiverIndex Index into fpTransceivers[] (0..2).
     * \return Number of frames received into the software queue.
     * \note Called from CAN RX ISR context. Not reentrant.
     *       transceiverIndex maps directly to the fpTransceivers[] array.
     */
    static uint8_t receiveInterrupt(uint8_t transceiverIndex);

    /**
     * \brief Handle CAN TX complete interrupt for a given transceiver.
     * \param transceiverIndex Index into fpTransceivers[] (0..2).
     * \note Called from CAN TX ISR context. Not reentrant.
     */
    static void transmitInterrupt(uint8_t transceiverIndex);

    /**
     * \brief Disable the RX FIFO interrupt for a given transceiver.
     * \param transceiverIndex Index into fpTransceivers[] (0..2).
     * \note Called from ISR context to prevent re-entry during FIFO drain.
     */
    static void disableRxInterrupt(uint8_t transceiverIndex);

    /**
     * \brief Re-enable the RX FIFO interrupt for a given transceiver.
     * \param transceiverIndex Index into fpTransceivers[] (0..2).
     * \note Called from async task context after FIFO processing is complete.
     */
    static void enableRxInterrupt(uint8_t transceiverIndex);

    /**
     * \brief Periodic task — checks bus-off state and updates transceiver state.
     *
     * If bus-off is detected while OPEN, transitions to MUTED.
     * If bus-off clears and the user did not explicitly mute, transitions back to OPEN.
     */
    void cyclicTask();

    /**
     * \brief Drain the software RX queue and notify frame listeners.
     *
     * Iterates over buffered frames from the BxCanDevice, notifies registered
     * listeners, clears the queue, and re-enables the RX FIFO interrupt.
     */
    void receiveTask();

    /// Underlying bxCAN hardware device.
    /// Public for test access (mock injection via `fFct.fDevice`).
    BxCanDevice fDevice;

private:
    static BxCanTransceiver*
        fpTransceivers[3]; ///< Transceiver instances indexed by busId (CAN1, CAN2, CAN3).

    ::async::ContextType fContext;       ///< Async execution context.
    ::async::TimeoutType fCyclicTimeout; ///< Timeout handle for cyclic task scheduling.
    bool fMuted;                         ///< True if explicitly muted by the user.

    /**
     * \brief Callback invoked from async context after TX ISR completes.
     */
    void canFrameSentCallback();
};

} // namespace bios

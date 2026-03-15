// Copyright 2024 Contributors to the Eclipse Foundation
//
// SPDX-License-Identifier: EPL-2.0

#include "can/transceiver/fdcan/FdCanTransceiver.h"

#include "async/AsyncMock.h"
#include "bsp/timer/SystemTimerMock.h"
#include "can/canframes/ICANFrameSentListener.h"

#include <gtest/gtest.h>

namespace
{
using namespace ::can;
using namespace ::testing;
using namespace ::bios;

class FdCanTransceiverTest : public Test
{
public:
    ::async::AsyncMock fAsyncMock;
    ::async::ContextType fAsyncContext = 0;
    uint8_t fBusId                     = 0;
    FdCanDevice::Config fDevConfig{};
};

/// \brief init() transitions from CLOSED to INITIALIZED
TEST_F(FdCanTransceiverTest, initFromClosed)
{
    FdCanTransceiver fct(fAsyncContext, fBusId, fDevConfig);

    EXPECT_EQ(ICanTransceiver::ErrorCode::CAN_ERR_OK, fct.init());
}

/// \brief Repeated init() calls should fail with ILLEGAL_STATE
TEST_F(FdCanTransceiverTest, initTwiceFails)
{
    FdCanTransceiver fct(fAsyncContext, fBusId, fDevConfig);

    EXPECT_EQ(ICanTransceiver::ErrorCode::CAN_ERR_OK, fct.init());
    EXPECT_EQ(ICanTransceiver::ErrorCode::CAN_ERR_ILLEGAL_STATE, fct.init());
}

class InitedFdCanTransceiverTest : public FdCanTransceiverTest
{
public:
    InitedFdCanTransceiverTest() : fFct(fAsyncContext, fBusId, fDevConfig)
    {
        EXPECT_CALL(fFct.fDevice, init()).Times(AnyNumber());
        EXPECT_CALL(fFct.fDevice, start()).Times(AnyNumber());
        EXPECT_CALL(fFct.fDevice, stop()).Times(AnyNumber());
        EXPECT_CALL(fAsyncMock, execute(fAsyncContext, _))
            .Times(AnyNumber())
            .WillRepeatedly([](auto, auto& runnable) { runnable.execute(); });

        EXPECT_EQ(ICanTransceiver::ErrorCode::CAN_ERR_OK, fFct.init());
    }

    FdCanTransceiver fFct;
};

/// \brief open() transitions from INITIALIZED to OPEN
TEST_F(InitedFdCanTransceiverTest, open)
{
    EXPECT_EQ(ICanTransceiver::ErrorCode::CAN_ERR_OK, fFct.open());
}

/// \brief Repeated open() calls should fail
TEST_F(InitedFdCanTransceiverTest, openTwiceFails)
{
    EXPECT_EQ(ICanTransceiver::ErrorCode::CAN_ERR_OK, fFct.open());
    EXPECT_EQ(ICanTransceiver::ErrorCode::CAN_ERR_ILLEGAL_STATE, fFct.open());
}

/// \brief open() from CLOSED re-initializes device
TEST_F(InitedFdCanTransceiverTest, openFromClosed)
{
    EXPECT_EQ(ICanTransceiver::ErrorCode::CAN_ERR_OK, fFct.open());
    EXPECT_EQ(ICanTransceiver::ErrorCode::CAN_ERR_OK, fFct.close());
    EXPECT_EQ(ICanTransceiver::ErrorCode::CAN_ERR_OK, fFct.open());
}

/// \brief close() from OPEN returns OK
TEST_F(InitedFdCanTransceiverTest, closeFromOpen)
{
    EXPECT_EQ(ICanTransceiver::ErrorCode::CAN_ERR_OK, fFct.open());
    EXPECT_EQ(ICanTransceiver::ErrorCode::CAN_ERR_OK, fFct.close());
}

/// \brief close() when already CLOSED returns OK
TEST_F(InitedFdCanTransceiverTest, closeFromClosedReturnsOk)
{
    EXPECT_EQ(ICanTransceiver::ErrorCode::CAN_ERR_OK, fFct.open());
    EXPECT_EQ(ICanTransceiver::ErrorCode::CAN_ERR_OK, fFct.close());
    EXPECT_EQ(ICanTransceiver::ErrorCode::CAN_ERR_OK, fFct.close());
}

/// \brief mute() from OPEN returns OK
TEST_F(InitedFdCanTransceiverTest, muteFromOpen)
{
    EXPECT_EQ(ICanTransceiver::ErrorCode::CAN_ERR_OK, fFct.open());
    EXPECT_EQ(ICanTransceiver::ErrorCode::CAN_ERR_OK, fFct.mute());
}

/// \brief mute() from INITIALIZED should fail
TEST_F(InitedFdCanTransceiverTest, muteFromInitializedFails)
{
    EXPECT_EQ(ICanTransceiver::ErrorCode::CAN_ERR_ILLEGAL_STATE, fFct.mute());
}

/// \brief unmute() from MUTED returns OK
TEST_F(InitedFdCanTransceiverTest, unmuteFromMuted)
{
    EXPECT_EQ(ICanTransceiver::ErrorCode::CAN_ERR_OK, fFct.open());
    EXPECT_EQ(ICanTransceiver::ErrorCode::CAN_ERR_OK, fFct.mute());
    EXPECT_EQ(ICanTransceiver::ErrorCode::CAN_ERR_OK, fFct.unmute());
}

/// \brief unmute() from OPEN should fail
TEST_F(InitedFdCanTransceiverTest, unmuteFromOpenFails)
{
    EXPECT_EQ(ICanTransceiver::ErrorCode::CAN_ERR_OK, fFct.open());
    EXPECT_EQ(ICanTransceiver::ErrorCode::CAN_ERR_ILLEGAL_STATE, fFct.unmute());
}

/// \brief write() when OPEN calls transmit on device
TEST_F(InitedFdCanTransceiverTest, writeWhenOpen)
{
    CANFrame frame;

    EXPECT_CALL(fFct.fDevice, transmit(_)).WillOnce(Return(true));

    EXPECT_EQ(ICanTransceiver::ErrorCode::CAN_ERR_OK, fFct.open());
    EXPECT_EQ(ICanTransceiver::ErrorCode::CAN_ERR_OK, fFct.write(frame));
}

/// \brief write() when MUTED returns TX_OFFLINE
TEST_F(InitedFdCanTransceiverTest, writeWhenMutedFails)
{
    CANFrame frame;

    EXPECT_EQ(ICanTransceiver::ErrorCode::CAN_ERR_OK, fFct.open());
    EXPECT_EQ(ICanTransceiver::ErrorCode::CAN_ERR_OK, fFct.mute());
    EXPECT_EQ(ICanTransceiver::ErrorCode::CAN_ERR_TX_OFFLINE, fFct.write(frame));
}

/// \brief write() when TX FIFO full returns HW_QUEUE_FULL
TEST_F(InitedFdCanTransceiverTest, writeWhenFifoFull)
{
    CANFrame frame;

    EXPECT_CALL(fFct.fDevice, transmit(_)).WillOnce(Return(false));

    EXPECT_EQ(ICanTransceiver::ErrorCode::CAN_ERR_OK, fFct.open());
    EXPECT_EQ(ICanTransceiver::ErrorCode::CAN_ERR_TX_HW_QUEUE_FULL, fFct.write(frame));
}

/// \brief getBaudrate() returns expected value
TEST_F(InitedFdCanTransceiverTest, getBaudrate) { EXPECT_EQ(500000U, fFct.getBaudrate()); }

/// \brief getHwQueueTimeout() returns expected value
TEST_F(InitedFdCanTransceiverTest, getHwQueueTimeout) { EXPECT_EQ(10U, fFct.getHwQueueTimeout()); }

/// \brief receiveTask() drains RX queue and notifies listeners
TEST_F(InitedFdCanTransceiverTest, receiveTask)
{
    CANFrame frame;

    EXPECT_CALL(fFct.fDevice, getRxCount()).WillOnce(Return(0));
    EXPECT_CALL(fFct.fDevice, clearRxQueue()).Times(1);

    fFct.receiveTask();
}

/// \brief cyclicTask() detects bus-off and transitions to MUTED
TEST_F(InitedFdCanTransceiverTest, cyclicTaskBusOff)
{
    EXPECT_EQ(ICanTransceiver::ErrorCode::CAN_ERR_OK, fFct.open());

    EXPECT_CALL(fFct.fDevice, isBusOff()).WillOnce(Return(true));
    fFct.cyclicTask();

    // After bus-off, write should fail
    CANFrame frame;
    EXPECT_EQ(ICanTransceiver::ErrorCode::CAN_ERR_TX_OFFLINE, fFct.write(frame));
}

/// \brief cyclicTask() recovers from bus-off automatically
TEST_F(InitedFdCanTransceiverTest, cyclicTaskBusRecovery)
{
    EXPECT_EQ(ICanTransceiver::ErrorCode::CAN_ERR_OK, fFct.open());

    EXPECT_CALL(fFct.fDevice, isBusOff()).WillOnce(Return(true)).WillOnce(Return(false));

    fFct.cyclicTask(); // bus-off -> MUTED
    fFct.cyclicTask(); // bus-on -> OPEN

    EXPECT_CALL(fFct.fDevice, transmit(_)).WillOnce(Return(true));
    CANFrame frame;
    EXPECT_EQ(ICanTransceiver::ErrorCode::CAN_ERR_OK, fFct.write(frame));
}

/// \brief shutdown() calls close
TEST_F(InitedFdCanTransceiverTest, shutdown)
{
    EXPECT_EQ(ICanTransceiver::ErrorCode::CAN_ERR_OK, fFct.open());
    fFct.shutdown();
}

/// \brief receiveInterrupt() with valid index calls device receiveISR
TEST_F(InitedFdCanTransceiverTest, receiveInterrupt)
{
    EXPECT_CALL(fFct.fDevice, receiveISR(_)).WillOnce(Return(0));
    FdCanTransceiver::receiveInterrupt(fBusId);
}

/// \brief transmitInterrupt() with valid index calls device transmitISR
TEST_F(InitedFdCanTransceiverTest, transmitInterrupt)
{
    EXPECT_CALL(fFct.fDevice, transmitISR());
    FdCanTransceiver::transmitInterrupt(fBusId);
}

/// \brief receiveInterrupt() with invalid index returns 0
TEST_F(FdCanTransceiverTest, receiveInterruptInvalidIndex)
{
    EXPECT_EQ(0U, FdCanTransceiver::receiveInterrupt(3));
}

} // namespace

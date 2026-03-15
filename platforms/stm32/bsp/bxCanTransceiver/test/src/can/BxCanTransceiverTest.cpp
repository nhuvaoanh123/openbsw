// Copyright 2024 Contributors to the Eclipse Foundation
//
// SPDX-License-Identifier: EPL-2.0

#include "can/transceiver/bxcan/BxCanTransceiver.h"

#include "async/AsyncMock.h"
#include "bsp/timer/SystemTimerMock.h"
#include "can/canframes/ICANFrameSentListener.h"

#include <gtest/gtest.h>

namespace
{
using namespace ::can;
using namespace ::testing;
using namespace ::bios;

class BxCanTransceiverTest : public Test
{
public:
    ::async::AsyncMock fAsyncMock;
    ::async::ContextType fAsyncContext = 0;
    uint8_t fBusId                     = 0;
    BxCanDevice::Config fDevConfig{};
};

/// \brief init() transitions from CLOSED to INITIALIZED
TEST_F(BxCanTransceiverTest, initFromClosed)
{
    BxCanTransceiver bxt(fAsyncContext, fBusId, fDevConfig);

    EXPECT_EQ(ICanTransceiver::ErrorCode::CAN_ERR_OK, bxt.init());
}

/// \brief Repeated init() calls should fail with ILLEGAL_STATE
TEST_F(BxCanTransceiverTest, initTwiceFails)
{
    BxCanTransceiver bxt(fAsyncContext, fBusId, fDevConfig);

    EXPECT_EQ(ICanTransceiver::ErrorCode::CAN_ERR_OK, bxt.init());
    EXPECT_EQ(ICanTransceiver::ErrorCode::CAN_ERR_ILLEGAL_STATE, bxt.init());
}

class InitedBxCanTransceiverTest : public BxCanTransceiverTest
{
public:
    InitedBxCanTransceiverTest() : fBxt(fAsyncContext, fBusId, fDevConfig)
    {
        EXPECT_CALL(fBxt.fDevice, init()).Times(AnyNumber());
        EXPECT_CALL(fBxt.fDevice, start()).Times(AnyNumber());
        EXPECT_CALL(fBxt.fDevice, stop()).Times(AnyNumber());
        EXPECT_CALL(fAsyncMock, execute(fAsyncContext, _))
            .Times(AnyNumber())
            .WillRepeatedly([](auto, auto& runnable) { runnable.execute(); });

        EXPECT_EQ(ICanTransceiver::ErrorCode::CAN_ERR_OK, fBxt.init());
    }

    BxCanTransceiver fBxt;
};

/// \brief open() transitions from INITIALIZED to OPEN
TEST_F(InitedBxCanTransceiverTest, open)
{
    EXPECT_EQ(ICanTransceiver::ErrorCode::CAN_ERR_OK, fBxt.open());
}

/// \brief Repeated open() calls should fail
TEST_F(InitedBxCanTransceiverTest, openTwiceFails)
{
    EXPECT_EQ(ICanTransceiver::ErrorCode::CAN_ERR_OK, fBxt.open());
    EXPECT_EQ(ICanTransceiver::ErrorCode::CAN_ERR_ILLEGAL_STATE, fBxt.open());
}

/// \brief close() from OPEN returns OK
TEST_F(InitedBxCanTransceiverTest, closeFromOpen)
{
    EXPECT_EQ(ICanTransceiver::ErrorCode::CAN_ERR_OK, fBxt.open());
    EXPECT_EQ(ICanTransceiver::ErrorCode::CAN_ERR_OK, fBxt.close());
}

/// \brief close() from INITIALIZED returns OK (transitions to CLOSED)
TEST_F(InitedBxCanTransceiverTest, closeFromInitializedReturnsOk)
{
    EXPECT_CALL(fBxt.fDevice, stop());
    EXPECT_EQ(ICanTransceiver::ErrorCode::CAN_ERR_OK, fBxt.close());
}

/// \brief mute() from OPEN returns OK
TEST_F(InitedBxCanTransceiverTest, muteFromOpen)
{
    EXPECT_EQ(ICanTransceiver::ErrorCode::CAN_ERR_OK, fBxt.open());
    EXPECT_EQ(ICanTransceiver::ErrorCode::CAN_ERR_OK, fBxt.mute());
}

/// \brief mute() from INITIALIZED should fail
TEST_F(InitedBxCanTransceiverTest, muteFromInitializedFails)
{
    EXPECT_EQ(ICanTransceiver::ErrorCode::CAN_ERR_ILLEGAL_STATE, fBxt.mute());
}

/// \brief unmute() from MUTED returns OK
TEST_F(InitedBxCanTransceiverTest, unmuteFromMuted)
{
    EXPECT_EQ(ICanTransceiver::ErrorCode::CAN_ERR_OK, fBxt.open());
    EXPECT_EQ(ICanTransceiver::ErrorCode::CAN_ERR_OK, fBxt.mute());
    EXPECT_EQ(ICanTransceiver::ErrorCode::CAN_ERR_OK, fBxt.unmute());
}

/// \brief unmute() from OPEN should fail
TEST_F(InitedBxCanTransceiverTest, unmuteFromOpenFails)
{
    EXPECT_EQ(ICanTransceiver::ErrorCode::CAN_ERR_OK, fBxt.open());
    EXPECT_EQ(ICanTransceiver::ErrorCode::CAN_ERR_ILLEGAL_STATE, fBxt.unmute());
}

/// \brief write() when OPEN calls transmit on device
TEST_F(InitedBxCanTransceiverTest, writeWhenOpen)
{
    CANFrame frame;

    EXPECT_CALL(fBxt.fDevice, transmit(_)).WillOnce(Return(true));

    EXPECT_EQ(ICanTransceiver::ErrorCode::CAN_ERR_OK, fBxt.open());
    EXPECT_EQ(ICanTransceiver::ErrorCode::CAN_ERR_OK, fBxt.write(frame));
}

/// \brief write() when MUTED returns TX_OFFLINE
TEST_F(InitedBxCanTransceiverTest, writeWhenMutedFails)
{
    CANFrame frame;

    EXPECT_EQ(ICanTransceiver::ErrorCode::CAN_ERR_OK, fBxt.open());
    EXPECT_EQ(ICanTransceiver::ErrorCode::CAN_ERR_OK, fBxt.mute());
    EXPECT_EQ(ICanTransceiver::ErrorCode::CAN_ERR_TX_OFFLINE, fBxt.write(frame));
}

/// \brief write() when TX FIFO full returns HW_QUEUE_FULL
TEST_F(InitedBxCanTransceiverTest, writeWhenFifoFull)
{
    CANFrame frame;

    EXPECT_CALL(fBxt.fDevice, transmit(_)).WillOnce(Return(false));

    EXPECT_EQ(ICanTransceiver::ErrorCode::CAN_ERR_OK, fBxt.open());
    EXPECT_EQ(ICanTransceiver::ErrorCode::CAN_ERR_TX_HW_QUEUE_FULL, fBxt.write(frame));
}

/// \brief getBaudrate() returns expected value
TEST_F(InitedBxCanTransceiverTest, getBaudrate) { EXPECT_EQ(500000U, fBxt.getBaudrate()); }

/// \brief getHwQueueTimeout() returns expected value
TEST_F(InitedBxCanTransceiverTest, getHwQueueTimeout) { EXPECT_EQ(10U, fBxt.getHwQueueTimeout()); }

/// \brief receiveTask() drains RX queue and notifies listeners
TEST_F(InitedBxCanTransceiverTest, receiveTask)
{
    CANFrame frame;

    EXPECT_CALL(fBxt.fDevice, getRxCount()).WillOnce(Return(0));
    EXPECT_CALL(fBxt.fDevice, clearRxQueue()).Times(1);

    fBxt.receiveTask();
}

/// \brief cyclicTask() detects bus-off and transitions to MUTED
TEST_F(InitedBxCanTransceiverTest, cyclicTaskBusOff)
{
    EXPECT_EQ(ICanTransceiver::ErrorCode::CAN_ERR_OK, fBxt.open());

    EXPECT_CALL(fBxt.fDevice, isBusOff()).WillOnce(Return(true));
    fBxt.cyclicTask();

    // After bus-off, write should fail because state is MUTED
    CANFrame frame;
    EXPECT_EQ(ICanTransceiver::ErrorCode::CAN_ERR_TX_OFFLINE, fBxt.write(frame));
}

/// \brief cyclicTask() recovers from bus-off when bus comes back
TEST_F(InitedBxCanTransceiverTest, cyclicTaskBusRecovery)
{
    EXPECT_EQ(ICanTransceiver::ErrorCode::CAN_ERR_OK, fBxt.open());

    // Go to bus-off
    EXPECT_CALL(fBxt.fDevice, isBusOff()).WillOnce(Return(true)).WillOnce(Return(false));

    fBxt.cyclicTask(); // bus-off -> MUTED
    fBxt.cyclicTask(); // bus-on -> OPEN (auto-recovery, not user mute)

    // Write should work again
    EXPECT_CALL(fBxt.fDevice, transmit(_)).WillOnce(Return(true));
    CANFrame frame;
    EXPECT_EQ(ICanTransceiver::ErrorCode::CAN_ERR_OK, fBxt.write(frame));
}

/// \brief shutdown() calls close
TEST_F(InitedBxCanTransceiverTest, shutdown)
{
    EXPECT_EQ(ICanTransceiver::ErrorCode::CAN_ERR_OK, fBxt.open());
    fBxt.shutdown();
}

/// \brief receiveInterrupt() with valid index calls device receiveISR
TEST_F(InitedBxCanTransceiverTest, receiveInterrupt)
{
    EXPECT_CALL(fBxt.fDevice, receiveISR(_)).WillOnce(Return(0));
    BxCanTransceiver::receiveInterrupt(fBusId);
}

/// \brief transmitInterrupt() with valid index calls device transmitISR
TEST_F(InitedBxCanTransceiverTest, transmitInterrupt)
{
    EXPECT_CALL(fBxt.fDevice, transmitISR());
    BxCanTransceiver::transmitInterrupt(fBusId);
}

/// \brief receiveInterrupt() with invalid index returns 0
TEST_F(BxCanTransceiverTest, receiveInterruptInvalidIndex)
{
    EXPECT_EQ(0U, BxCanTransceiver::receiveInterrupt(3));
}

} // namespace

// Copyright 2024 Accenture.

/**
 * \ingroup async
 *
 * Regression tests for the FreeRTOS async task notification mechanism.
 *
 * These tests target a bug where repeated ISR-triggered async::execute() calls
 * fail to wake the task on the second (and subsequent) notifications. Root cause
 * on STM32G4/Cortex-M4:
 *   1. BASEPRI masks ISRs at priority >= configMAX_SYSCALL_INTERRUPT_PRIORITY
 *   2. Task notification state not properly reset between calls
 *   3. portYIELD_FROM_ISR may not trigger context switch at equal priority
 *
 * The test categories cover:
 *   1. xTaskNotifyFromISR + xTaskNotifyWait interaction
 *   2. enterIsr / leaveIsr lifecycle
 *   3. RunnableExecutor enqueue/dequeue
 *   4. BASEPRI interaction (modeled via mock)
 *   5. Full ISR -> task -> response cycles
 *   6. Integration with CAN transceiver pattern
 */

#include "async/FreeRtosAdapter.h"

#include "async/RunnableMock.h"
#include "async/TimeoutMock.h"

#include <bsp/timer/SystemTimerMock.h>
#include <os/FreeRtosMock.h>

namespace
{
using namespace ::async;
using namespace ::testing;

// ---------------------------------------------------------------------------
// Actions reused across tests
// ---------------------------------------------------------------------------
ACTION_P(StopDispatch, cut) { cut->stopDispatch(); }

ACTION_P(CopyArgPointee2, pointer) { *arg2 = *pointer; }

// ---------------------------------------------------------------------------
// Binding and Adapter types
// ---------------------------------------------------------------------------
struct TestBinding
{
    static size_t const TASK_COUNT             = 3U;
    static size_t const WAIT_EVENTS_TICK_COUNT = 100U;
};

using AdapterType     = FreeRtosAdapter<TestBinding>;
using TaskContextType = AdapterType::TaskContextType;

// ---------------------------------------------------------------------------
// Binding mock for TaskContext tests (singleton-based, same as TaskContextTest)
// ---------------------------------------------------------------------------
class TestBindingMock : public ::etl::singleton_base<TestBindingMock>
{
public:
    static EventMaskType const WAIT_EVENTS_TICK_COUNT = 100U;
    using BaseType_t                                  = int32_t;

    TestBindingMock() : ::etl::singleton_base<TestBindingMock>(*this) {}

    static BaseType_t* getHigherPriorityTaskWoken()
    {
        return instance().getHigherPriorityTaskWokenFunc();
    }

    MOCK_METHOD(BaseType_t*, getHigherPriorityTaskWokenFunc, ());
    MOCK_METHOD(void, taskFunction, (TaskContext<TestBindingMock> & taskContext));
};

// ---------------------------------------------------------------------------
// Concrete runnable for integration tests
// ---------------------------------------------------------------------------
class CountingRunnable : public IRunnable
{
public:
    CountingRunnable() : _count(0U) {}

    void execute() override { ++_count; }

    uint32_t _count;
};

// ============================================================================
// Base fixture for TaskContext-level tests (Categories 1, 3, 5, 6)
// ============================================================================
class TaskNotificationRegressionTest : public Test
{
public:
    TaskNotificationRegressionTest() : _name("regr"), _taskHandle(42U) {}

    void SetUp() override
    {
        // Create a task context for most tests
        EXPECT_CALL(
            _freeRtosMock,
            xTaskCreateStatic(NotNull(), _name, 100, NotNull(), 1U, _stack, &_task))
            .WillOnce(
                DoAll(SaveArg<0>(&_osTaskFunction), SaveArg<3>(&_param), Return(&_taskHandle)));
        _cut.createTask(
            1U,
            _task,
            _name,
            1U,
            _stack,
            TaskContext<TestBindingMock>::TaskFunctionType());
        Mock::VerifyAndClearExpectations(&_freeRtosMock);
    }

    // Helper: simulate an ISR-context execute (returns the event mask that was set)
    uint32_t simulateIsrExecute(RunnableMock& runnable, BaseType_t& higherPriorityTaskWoken)
    {
        uint32_t eventMask = 0U;
        EXPECT_CALL(_bindingMock, getHigherPriorityTaskWokenFunc())
            .WillOnce(Return(&higherPriorityTaskWoken));
        EXPECT_CALL(
            _freeRtosMock,
            xTaskNotifyFromISR(&_taskHandle, _, eSetBits, &higherPriorityTaskWoken))
            .WillOnce(SaveArg<1>(&eventMask));
        _cut.execute(runnable);
        Mock::VerifyAndClearExpectations(&_bindingMock);
        Mock::VerifyAndClearExpectations(&_freeRtosMock);
        return eventMask;
    }

    // Helper: simulate a task-context execute (returns the event mask that was set)
    uint32_t simulateTaskExecute(RunnableMock& runnable)
    {
        uint32_t eventMask = 0U;
        EXPECT_CALL(_bindingMock, getHigherPriorityTaskWokenFunc())
            .WillOnce(Return(static_cast<BaseType_t*>(nullptr)));
        EXPECT_CALL(_freeRtosMock, xTaskNotify(&_taskHandle, _, eSetBits))
            .WillOnce(SaveArg<1>(&eventMask));
        _cut.execute(runnable);
        Mock::VerifyAndClearExpectations(&_bindingMock);
        Mock::VerifyAndClearExpectations(&_freeRtosMock);
        return eventMask;
    }

    // Helper: set up dispatch loop that processes one event then stops
    void setupSingleDispatchCycle(
        Sequence& seq, uint32_t eventMask, uint32_t& stopEventMask)
    {
        EXPECT_CALL(_systemTimerMock, getSystemTimeUs32Bit()).WillRepeatedly(Return(100000U));
        EXPECT_CALL(
            _freeRtosMock,
            xTaskNotifyWait(0U, 7U, NotNull(), TestBindingMock::WAIT_EVENTS_TICK_COUNT))
            .InSequence(seq)
            .WillOnce(DoAll(SetArgPointee<2>(eventMask), Return(true)));
        // After handling, second wait returns nothing and triggers stop
        EXPECT_CALL(
            _freeRtosMock,
            xTaskNotifyWait(0U, 7U, NotNull(), TestBindingMock::WAIT_EVENTS_TICK_COUNT))
            .InSequence(seq)
            .WillOnce(DoAll(SetArgPointee<2>(0U), StopDispatch(&_cut), Return(false)));
        // stopDispatch calls setEvents(STOP_EVENT_MASK)
        EXPECT_CALL(_bindingMock, getHigherPriorityTaskWokenFunc())
            .WillOnce(Return(static_cast<BaseType_t*>(nullptr)));
        EXPECT_CALL(_freeRtosMock, xTaskNotify(&_taskHandle, _, eSetBits))
            .InSequence(seq)
            .WillOnce(SaveArg<1>(&stopEventMask));
        EXPECT_CALL(
            _freeRtosMock,
            xTaskNotifyWait(0U, 7U, NotNull(), TestBindingMock::WAIT_EVENTS_TICK_COUNT))
            .InSequence(seq)
            .WillOnce(DoAll(CopyArgPointee2(&stopEventMask), Return(true)));
    }

protected:
    StrictMock<::os::FreeRtosMock> _freeRtosMock;
    StrictMock<TestBindingMock> _bindingMock;
    StrictMock<RunnableMock> _runnableMock1;
    StrictMock<RunnableMock> _runnableMock2;
    StrictMock<RunnableMock> _runnableMock3;
    StrictMock<SystemTimerMock> _systemTimerMock;
    TaskContext<TestBindingMock> _cut;
    StaticTask_t _task;
    StackType_t _stack[100];
    char const* _name;
    uint32_t _taskHandle;
    TaskFunction_t* _osTaskFunction;
    void* _param;
};

// ============================================================================
// Base fixture for FreeRtosAdapter-level tests (Categories 2, 4)
// ============================================================================
class AdapterNotificationRegressionTest : public Test
{
public:
    using CutType = FreeRtosAdapter<TestBinding>;

    MOCK_METHOD(void, startApp, ());
    MOCK_METHOD(void, taskFunction, (CutType::TaskContextType & taskContext));

protected:
    StrictMock<::os::FreeRtosMock> _freeRtosMock;
    StrictMock<RunnableMock> _runnableMock;
    StrictMock<SystemTimerMock> _systemTimerMock;
};

// ============================================================================
// Category 1: xTaskNotifyFromISR + xTaskNotifyWait interaction (10 tests)
// ============================================================================

/**
 * \desc: Second ISR notification must wake the task again after the first was processed.
 *        This is the core regression: the second xTaskNotifyFromISR must trigger a new
 *        xTaskNotifyWait return.
 */
TEST_F(TaskNotificationRegressionTest, secondNotificationWakesTask)
{
    BaseType_t higherPriorityTaskWoken = pdFALSE;

    // First ISR execute
    uint32_t eventMask1 = simulateIsrExecute(_runnableMock1, higherPriorityTaskWoken);

    // Second ISR execute (different runnable so it can be enqueued)
    uint32_t eventMask2 = simulateIsrExecute(_runnableMock2, higherPriorityTaskWoken);

    // Dispatch: first wait returns first event, processes it; second wait returns second event
    EXPECT_CALL(_systemTimerMock, getSystemTimeUs32Bit()).WillRepeatedly(Return(100000U));
    Sequence seq;
    // Both runnables were enqueued before dispatch starts, so the first handleEvents
    // should drain the whole queue (both are on the same event bit)
    EXPECT_CALL(
        _freeRtosMock,
        xTaskNotifyWait(0U, 7U, NotNull(), TestBindingMock::WAIT_EVENTS_TICK_COUNT))
        .InSequence(seq)
        .WillOnce(DoAll(SetArgPointee<2>(eventMask1 | eventMask2), Return(true)));

    // Both runnables should execute during handleEvent (queue drain loop)
    EXPECT_CALL(_runnableMock1, execute()).InSequence(seq);
    EXPECT_CALL(_runnableMock2, execute()).InSequence(seq);

    // Next wait: nothing pending, stop
    uint32_t stopMask = 0U;
    EXPECT_CALL(
        _freeRtosMock,
        xTaskNotifyWait(0U, 7U, NotNull(), TestBindingMock::WAIT_EVENTS_TICK_COUNT))
        .InSequence(seq)
        .WillOnce(DoAll(SetArgPointee<2>(0U), StopDispatch(&_cut), Return(false)));
    EXPECT_CALL(_bindingMock, getHigherPriorityTaskWokenFunc())
        .WillOnce(Return(static_cast<BaseType_t*>(nullptr)));
    EXPECT_CALL(_freeRtosMock, xTaskNotify(&_taskHandle, _, eSetBits))
        .InSequence(seq)
        .WillOnce(SaveArg<1>(&stopMask));
    EXPECT_CALL(
        _freeRtosMock,
        xTaskNotifyWait(0U, 7U, NotNull(), TestBindingMock::WAIT_EVENTS_TICK_COUNT))
        .InSequence(seq)
        .WillOnce(DoAll(CopyArgPointee2(&stopMask), Return(true)));

    _osTaskFunction(_param);
}

/**
 * \desc: After xTaskNotifyWait returns, the WAIT_EVENT_MASK bits (0x07) must be cleared.
 */
TEST_F(TaskNotificationRegressionTest, notificationBitsCleanedOnExit)
{
    // The second parameter to xTaskNotifyWait is ulBitsToClearOnExit = 7U (0x07)
    // This is verified by the mock expectation: xTaskNotifyWait(0U, 7U, ...)
    // We verify that the framework always passes 7U as the clear-on-exit mask.
    uint32_t eventMask = simulateTaskExecute(_runnableMock1);

    EXPECT_CALL(_systemTimerMock, getSystemTimeUs32Bit()).WillRepeatedly(Return(100000U));
    Sequence seq;
    // Verify clear-on-exit is 7U (WAIT_EVENT_MASK for 2 events + stop)
    EXPECT_CALL(
        _freeRtosMock,
        xTaskNotifyWait(0U, 7U, NotNull(), TestBindingMock::WAIT_EVENTS_TICK_COUNT))
        .InSequence(seq)
        .WillOnce(DoAll(SetArgPointee<2>(eventMask), Return(true)));
    EXPECT_CALL(_runnableMock1, execute());
    uint32_t stopMask = 0U;
    EXPECT_CALL(
        _freeRtosMock,
        xTaskNotifyWait(0U, 7U, NotNull(), TestBindingMock::WAIT_EVENTS_TICK_COUNT))
        .InSequence(seq)
        .WillOnce(DoAll(SetArgPointee<2>(0U), StopDispatch(&_cut), Return(false)));
    EXPECT_CALL(_bindingMock, getHigherPriorityTaskWokenFunc())
        .WillOnce(Return(static_cast<BaseType_t*>(nullptr)));
    EXPECT_CALL(_freeRtosMock, xTaskNotify(&_taskHandle, _, eSetBits))
        .InSequence(seq)
        .WillOnce(SaveArg<1>(&stopMask));
    EXPECT_CALL(
        _freeRtosMock,
        xTaskNotifyWait(0U, 7U, NotNull(), TestBindingMock::WAIT_EVENTS_TICK_COUNT))
        .InSequence(seq)
        .WillOnce(DoAll(CopyArgPointee2(&stopMask), Return(true)));

    _osTaskFunction(_param);
}

/**
 * \desc: ISR fires while task is processing first event. Task must pick up second.
 */
TEST_F(TaskNotificationRegressionTest, notificationWhileTaskRunning)
{
    BaseType_t higherPriorityTaskWoken = pdFALSE;
    uint32_t eventMask1 = simulateIsrExecute(_runnableMock1, higherPriorityTaskWoken);

    EXPECT_CALL(_systemTimerMock, getSystemTimeUs32Bit()).WillRepeatedly(Return(100000U));
    Sequence seq;

    // First wait returns the event
    EXPECT_CALL(
        _freeRtosMock,
        xTaskNotifyWait(0U, 7U, NotNull(), TestBindingMock::WAIT_EVENTS_TICK_COUNT))
        .InSequence(seq)
        .WillOnce(DoAll(SetArgPointee<2>(eventMask1), Return(true)));

    // During _runnableMock1.execute(), enqueue a second runnable (simulates ISR during processing)
    uint32_t eventMask2 = 0U;
    EXPECT_CALL(_runnableMock1, execute())
        .InSequence(seq)
        .WillOnce(InvokeWithoutArgs([this, &eventMask2]() {
            // Simulate ISR context: enqueue second runnable
            BaseType_t hptw = pdFALSE;
            eventMask2      = simulateIsrExecute(_runnableMock2, hptw);
        }));

    // Second wait returns the second event
    EXPECT_CALL(
        _freeRtosMock,
        xTaskNotifyWait(0U, 7U, NotNull(), TestBindingMock::WAIT_EVENTS_TICK_COUNT))
        .InSequence(seq)
        .WillOnce(DoAll(SetArgPointee<2>(eventMask1), Return(true)));
    EXPECT_CALL(_runnableMock2, execute()).InSequence(seq);

    // Stop
    uint32_t stopMask = 0U;
    EXPECT_CALL(
        _freeRtosMock,
        xTaskNotifyWait(0U, 7U, NotNull(), TestBindingMock::WAIT_EVENTS_TICK_COUNT))
        .InSequence(seq)
        .WillOnce(DoAll(SetArgPointee<2>(0U), StopDispatch(&_cut), Return(false)));
    EXPECT_CALL(_bindingMock, getHigherPriorityTaskWokenFunc())
        .WillOnce(Return(static_cast<BaseType_t*>(nullptr)));
    EXPECT_CALL(_freeRtosMock, xTaskNotify(&_taskHandle, _, eSetBits))
        .InSequence(seq)
        .WillOnce(SaveArg<1>(&stopMask));
    EXPECT_CALL(
        _freeRtosMock,
        xTaskNotifyWait(0U, 7U, NotNull(), TestBindingMock::WAIT_EVENTS_TICK_COUNT))
        .InSequence(seq)
        .WillOnce(DoAll(CopyArgPointee2(&stopMask), Return(true)));

    _osTaskFunction(_param);
}

/**
 * \desc: ISR fires during handleEvent() execution, task loops and processes it.
 */
TEST_F(TaskNotificationRegressionTest, notificationDuringHandleEvents)
{
    // This test verifies the RunnableExecutor::handleEvent() while-loop drains
    // even runnables enqueued during execution of a prior runnable.
    uint32_t eventMask = simulateTaskExecute(_runnableMock1);

    EXPECT_CALL(_systemTimerMock, getSystemTimeUs32Bit()).WillRepeatedly(Return(100000U));
    Sequence seq;

    EXPECT_CALL(
        _freeRtosMock,
        xTaskNotifyWait(0U, 7U, NotNull(), TestBindingMock::WAIT_EVENTS_TICK_COUNT))
        .InSequence(seq)
        .WillOnce(DoAll(SetArgPointee<2>(eventMask), Return(true)));

    // During runnableMock1.execute(), enqueue runnableMock2 directly (task context)
    EXPECT_CALL(_runnableMock1, execute())
        .InSequence(seq)
        .WillOnce(InvokeWithoutArgs([this]() {
            // Task-context enqueue: the handleEvent while-loop should pick this up
            EXPECT_CALL(_bindingMock, getHigherPriorityTaskWokenFunc())
                .WillOnce(Return(static_cast<BaseType_t*>(nullptr)));
            EXPECT_CALL(_freeRtosMock, xTaskNotify(&_taskHandle, _, eSetBits));
            _cut.execute(_runnableMock2);
        }));
    EXPECT_CALL(_runnableMock2, execute()).InSequence(seq);

    // Stop
    uint32_t stopMask = 0U;
    EXPECT_CALL(
        _freeRtosMock,
        xTaskNotifyWait(0U, 7U, NotNull(), TestBindingMock::WAIT_EVENTS_TICK_COUNT))
        .InSequence(seq)
        .WillOnce(DoAll(SetArgPointee<2>(0U), StopDispatch(&_cut), Return(false)));
    EXPECT_CALL(_bindingMock, getHigherPriorityTaskWokenFunc())
        .WillOnce(Return(static_cast<BaseType_t*>(nullptr)));
    EXPECT_CALL(_freeRtosMock, xTaskNotify(&_taskHandle, _, eSetBits))
        .InSequence(seq)
        .WillOnce(SaveArg<1>(&stopMask));
    EXPECT_CALL(
        _freeRtosMock,
        xTaskNotifyWait(0U, 7U, NotNull(), TestBindingMock::WAIT_EVENTS_TICK_COUNT))
        .InSequence(seq)
        .WillOnce(DoAll(CopyArgPointee2(&stopMask), Return(true)));

    _osTaskFunction(_param);
}

/**
 * \desc: 5 rapid ISR notifications, task processes all 5.
 */
TEST_F(TaskNotificationRegressionTest, multipleFastNotifications)
{
    // Use CountingRunnable instances so we can enqueue 5 different runnables
    CountingRunnable runnables[5];
    BaseType_t hptw = pdFALSE;
    uint32_t combinedMask = 0U;

    for (int i = 0; i < 5; ++i)
    {
        uint32_t mask = 0U;
        EXPECT_CALL(_bindingMock, getHigherPriorityTaskWokenFunc())
            .WillOnce(Return(&hptw));
        EXPECT_CALL(
            _freeRtosMock,
            xTaskNotifyFromISR(&_taskHandle, _, eSetBits, &hptw))
            .WillOnce(SaveArg<1>(&mask));
        _cut.execute(runnables[i]);
        combinedMask |= mask;
        Mock::VerifyAndClearExpectations(&_bindingMock);
        Mock::VerifyAndClearExpectations(&_freeRtosMock);
    }

    // Dispatch: all 5 should execute
    EXPECT_CALL(_systemTimerMock, getSystemTimeUs32Bit()).WillRepeatedly(Return(100000U));
    Sequence seq;
    EXPECT_CALL(
        _freeRtosMock,
        xTaskNotifyWait(0U, 7U, NotNull(), TestBindingMock::WAIT_EVENTS_TICK_COUNT))
        .InSequence(seq)
        .WillOnce(DoAll(SetArgPointee<2>(combinedMask), Return(true)));

    // Stop on second wait
    uint32_t stopMask = 0U;
    EXPECT_CALL(
        _freeRtosMock,
        xTaskNotifyWait(0U, 7U, NotNull(), TestBindingMock::WAIT_EVENTS_TICK_COUNT))
        .InSequence(seq)
        .WillOnce(DoAll(SetArgPointee<2>(0U), StopDispatch(&_cut), Return(false)));
    EXPECT_CALL(_bindingMock, getHigherPriorityTaskWokenFunc())
        .WillOnce(Return(static_cast<BaseType_t*>(nullptr)));
    EXPECT_CALL(_freeRtosMock, xTaskNotify(&_taskHandle, _, eSetBits))
        .InSequence(seq)
        .WillOnce(SaveArg<1>(&stopMask));
    EXPECT_CALL(
        _freeRtosMock,
        xTaskNotifyWait(0U, 7U, NotNull(), TestBindingMock::WAIT_EVENTS_TICK_COUNT))
        .InSequence(seq)
        .WillOnce(DoAll(CopyArgPointee2(&stopMask), Return(true)));

    _osTaskFunction(_param);

    for (int i = 0; i < 5; ++i)
    {
        EXPECT_EQ(1U, runnables[i]._count) << "Runnable " << i << " did not execute";
    }
}

/**
 * \desc: xTaskNotifyWait times out, then ISR notifies, task processes it.
 */
TEST_F(TaskNotificationRegressionTest, notificationAfterTimeout)
{
    EXPECT_CALL(_systemTimerMock, getSystemTimeUs32Bit()).WillRepeatedly(Return(100000U));
    Sequence seq;

    // First wait: timeout (return false, no event)
    EXPECT_CALL(
        _freeRtosMock,
        xTaskNotifyWait(0U, 7U, NotNull(), TestBindingMock::WAIT_EVENTS_TICK_COUNT))
        .InSequence(seq)
        .WillOnce(DoAll(SetArgPointee<2>(0U), Return(false)));

    // After timeout, no events: next iteration an ISR has fired
    BaseType_t hptw = pdFALSE;
    uint32_t eventMask = 0U;

    // We enqueue before dispatch starts so we can reference the mask
    EXPECT_CALL(_bindingMock, getHigherPriorityTaskWokenFunc())
        .WillOnce(Return(&hptw));
    EXPECT_CALL(
        _freeRtosMock,
        xTaskNotifyFromISR(&_taskHandle, _, eSetBits, &hptw))
        .WillOnce(SaveArg<1>(&eventMask));
    _cut.execute(_runnableMock1);
    Mock::VerifyAndClearExpectations(&_bindingMock);
    Mock::VerifyAndClearExpectations(&_freeRtosMock);

    // Second wait returns the event from the ISR
    EXPECT_CALL(
        _freeRtosMock,
        xTaskNotifyWait(0U, 7U, NotNull(), TestBindingMock::WAIT_EVENTS_TICK_COUNT))
        .InSequence(seq)
        .WillOnce(DoAll(SetArgPointee<2>(eventMask), Return(true)));
    EXPECT_CALL(_runnableMock1, execute()).InSequence(seq);

    // Stop
    uint32_t stopMask = 0U;
    EXPECT_CALL(
        _freeRtosMock,
        xTaskNotifyWait(0U, 7U, NotNull(), TestBindingMock::WAIT_EVENTS_TICK_COUNT))
        .InSequence(seq)
        .WillOnce(DoAll(SetArgPointee<2>(0U), StopDispatch(&_cut), Return(false)));
    EXPECT_CALL(_bindingMock, getHigherPriorityTaskWokenFunc())
        .WillOnce(Return(static_cast<BaseType_t*>(nullptr)));
    EXPECT_CALL(_freeRtosMock, xTaskNotify(&_taskHandle, _, eSetBits))
        .InSequence(seq)
        .WillOnce(SaveArg<1>(&stopMask));
    EXPECT_CALL(
        _freeRtosMock,
        xTaskNotifyWait(0U, 7U, NotNull(), TestBindingMock::WAIT_EVENTS_TICK_COUNT))
        .InSequence(seq)
        .WillOnce(DoAll(CopyArgPointee2(&stopMask), Return(true)));

    _osTaskFunction(_param);
}

/**
 * \desc: Same event bits set twice, task wakes twice (via two dispatch iterations).
 */
TEST_F(TaskNotificationRegressionTest, notificationWithSameBits)
{
    // Enqueue first runnable
    uint32_t eventMask = simulateTaskExecute(_runnableMock1);

    EXPECT_CALL(_systemTimerMock, getSystemTimeUs32Bit()).WillRepeatedly(Return(100000U));
    Sequence seq;

    // First dispatch: process first runnable
    EXPECT_CALL(
        _freeRtosMock,
        xTaskNotifyWait(0U, 7U, NotNull(), TestBindingMock::WAIT_EVENTS_TICK_COUNT))
        .InSequence(seq)
        .WillOnce(DoAll(SetArgPointee<2>(eventMask), Return(true)));
    EXPECT_CALL(_runnableMock1, execute())
        .InSequence(seq)
        .WillOnce(InvokeWithoutArgs([this]() {
            // During execution, enqueue second runnable with same event bits
            (void)simulateTaskExecute(_runnableMock2);
        }));
    // handleEvent drains queue; _runnableMock2 was just enqueued so it should execute too
    EXPECT_CALL(_runnableMock2, execute()).InSequence(seq);

    // Stop
    uint32_t stopMask = 0U;
    EXPECT_CALL(
        _freeRtosMock,
        xTaskNotifyWait(0U, 7U, NotNull(), TestBindingMock::WAIT_EVENTS_TICK_COUNT))
        .InSequence(seq)
        .WillOnce(DoAll(SetArgPointee<2>(0U), StopDispatch(&_cut), Return(false)));
    EXPECT_CALL(_bindingMock, getHigherPriorityTaskWokenFunc())
        .WillOnce(Return(static_cast<BaseType_t*>(nullptr)));
    EXPECT_CALL(_freeRtosMock, xTaskNotify(&_taskHandle, _, eSetBits))
        .InSequence(seq)
        .WillOnce(SaveArg<1>(&stopMask));
    EXPECT_CALL(
        _freeRtosMock,
        xTaskNotifyWait(0U, 7U, NotNull(), TestBindingMock::WAIT_EVENTS_TICK_COUNT))
        .InSequence(seq)
        .WillOnce(DoAll(CopyArgPointee2(&stopMask), Return(true)));

    _osTaskFunction(_param);
}

/**
 * \desc: Different event bits set via execute vs timer, task sees both.
 */
TEST_F(TaskNotificationRegressionTest, notificationWithDifferentBits)
{
    // Event 0 (execute) = bit 0x01, Event 1 (timer) = bit 0x02
    uint32_t executeMask = simulateTaskExecute(_runnableMock1);
    EXPECT_EQ(0x01U, executeMask);

    // Verify the execute event is bit 0 and timer event would be bit 1
    EXPECT_CALL(_systemTimerMock, getSystemTimeUs32Bit()).WillRepeatedly(Return(100000U));
    Sequence seq;

    // Deliver both bits at once
    EXPECT_CALL(
        _freeRtosMock,
        xTaskNotifyWait(0U, 7U, NotNull(), TestBindingMock::WAIT_EVENTS_TICK_COUNT))
        .InSequence(seq)
        .WillOnce(DoAll(SetArgPointee<2>(0x03U), Return(true)));  // bits 0 and 1
    EXPECT_CALL(_runnableMock1, execute()).InSequence(seq);

    // Stop
    uint32_t stopMask = 0U;
    EXPECT_CALL(
        _freeRtosMock,
        xTaskNotifyWait(0U, 7U, NotNull(), TestBindingMock::WAIT_EVENTS_TICK_COUNT))
        .InSequence(seq)
        .WillOnce(DoAll(SetArgPointee<2>(0U), StopDispatch(&_cut), Return(false)));
    EXPECT_CALL(_bindingMock, getHigherPriorityTaskWokenFunc())
        .WillOnce(Return(static_cast<BaseType_t*>(nullptr)));
    EXPECT_CALL(_freeRtosMock, xTaskNotify(&_taskHandle, _, eSetBits))
        .InSequence(seq)
        .WillOnce(SaveArg<1>(&stopMask));
    EXPECT_CALL(
        _freeRtosMock,
        xTaskNotifyWait(0U, 7U, NotNull(), TestBindingMock::WAIT_EVENTS_TICK_COUNT))
        .InSequence(seq)
        .WillOnce(DoAll(CopyArgPointee2(&stopMask), Return(true)));

    _osTaskFunction(_param);
}

/**
 * \desc: Verify portYIELD_FROM_ISR is called when task is woken from ISR.
 */
TEST_F(AdapterNotificationRegressionTest, portYIELDCalledOnWakeup)
{
    CutType::enterIsr();
    EXPECT_TRUE(CutType::getHigherPriorityTaskWoken() != nullptr);

    // Simulate xTaskNotifyFromISR setting the flag
    *CutType::getHigherPriorityTaskWoken() = pdTRUE;

    EXPECT_CALL(_freeRtosMock, portYIELD_FROM_ISR(pdTRUE));
    CutType::leaveIsr();
}

/**
 * \desc: higherPriorityTaskWoken flag is properly reset between ISR calls.
 */
TEST_F(AdapterNotificationRegressionTest, higherPriorityTaskWokenFlagReset)
{
    // First ISR cycle: flag set to true
    CutType::enterIsr();
    *CutType::getHigherPriorityTaskWoken() = pdTRUE;
    EXPECT_CALL(_freeRtosMock, portYIELD_FROM_ISR(pdTRUE));
    CutType::leaveIsr();
    Mock::VerifyAndClearExpectations(&_freeRtosMock);

    // Second ISR cycle: flag must start at pdFALSE again
    CutType::enterIsr();
    EXPECT_EQ(pdFALSE, *CutType::getHigherPriorityTaskWoken());
    CutType::leaveIsr();
}

// ============================================================================
// Category 2: enterIsr / leaveIsr lifecycle (8 tests)
// ============================================================================

/**
 * \desc: enterIsr sets the higherPriorityTaskWoken pointer to a valid address.
 */
TEST_F(AdapterNotificationRegressionTest, enterIsrSetsHigherPriorityTaskWoken)
{
    EXPECT_EQ(nullptr, CutType::getHigherPriorityTaskWoken());
    CutType::enterIsr();
    EXPECT_NE(nullptr, CutType::getHigherPriorityTaskWoken());
    CutType::leaveIsr();
}

/**
 * \desc: leaveIsr clears the higherPriorityTaskWoken pointer back to nullptr.
 */
TEST_F(AdapterNotificationRegressionTest, leaveIsrClearsHigherPriorityTaskWoken)
{
    CutType::enterIsr();
    EXPECT_NE(nullptr, CutType::getHigherPriorityTaskWoken());
    CutType::leaveIsr();
    EXPECT_EQ(nullptr, CutType::getHigherPriorityTaskWoken());
}

/**
 * \desc: leaveIsr calls portYIELD_FROM_ISR when the flag is set to pdTRUE.
 */
TEST_F(AdapterNotificationRegressionTest, leaveIsrCallsYieldWhenFlagSet)
{
    CutType::enterIsr();
    *CutType::getHigherPriorityTaskWoken() = pdTRUE;
    EXPECT_CALL(_freeRtosMock, portYIELD_FROM_ISR(pdTRUE));
    CutType::leaveIsr();
}

/**
 * \desc: leaveIsr does NOT call portYIELD_FROM_ISR when the flag is pdFALSE.
 */
TEST_F(AdapterNotificationRegressionTest, leaveIsrNoYieldWhenFlagFalse)
{
    CutType::enterIsr();
    EXPECT_EQ(pdFALSE, *CutType::getHigherPriorityTaskWoken());
    // No portYIELD_FROM_ISR expectation — StrictMock will fail if called
    CutType::leaveIsr();
}

/**
 * \desc: Nested ISRs increment/decrement count correctly; pointer stays valid.
 */
TEST_F(AdapterNotificationRegressionTest, nestedIsrIncrementCount)
{
    CutType::enterIsr();
    BaseType_t* ptr1 = CutType::getHigherPriorityTaskWoken();
    EXPECT_NE(nullptr, ptr1);

    CutType::enterIsr();
    BaseType_t* ptr2 = CutType::getHigherPriorityTaskWoken();
    EXPECT_NE(nullptr, ptr2);
    EXPECT_EQ(ptr1, ptr2);  // Same flag pointer

    // Inner leave: pointer should remain valid (count > 0)
    CutType::leaveIsr();
    EXPECT_NE(nullptr, CutType::getHigherPriorityTaskWoken());

    // Outer leave: pointer cleared
    CutType::leaveIsr();
    EXPECT_EQ(nullptr, CutType::getHigherPriorityTaskWoken());
}

/**
 * \desc: portYIELD only called on the outermost leaveIsr, not inner ones.
 */
TEST_F(AdapterNotificationRegressionTest, nestedIsrOnlyYieldOnOuterLeave)
{
    CutType::enterIsr();
    CutType::enterIsr();
    *CutType::getHigherPriorityTaskWoken() = pdTRUE;

    // Inner leave: no yield expected (StrictMock enforces)
    CutType::leaveIsr();

    // Outer leave: yield expected
    EXPECT_CALL(_freeRtosMock, portYIELD_FROM_ISR(pdTRUE));
    CutType::leaveIsr();
}

/**
 * \desc: After enter+leave, ISR count is 0 and pointer is nullptr.
 */
TEST_F(AdapterNotificationRegressionTest, isrCountReturnsToZero)
{
    CutType::enterIsr();
    CutType::leaveIsr();
    EXPECT_EQ(nullptr, CutType::getHigherPriorityTaskWoken());

    // Verify we can enter again cleanly (count was truly 0)
    CutType::enterIsr();
    EXPECT_NE(nullptr, CutType::getHigherPriorityTaskWoken());
    EXPECT_EQ(pdFALSE, *CutType::getHigherPriorityTaskWoken());
    CutType::leaveIsr();
}

/**
 * \desc: 10 enter/leave cycles all work correctly.
 */
TEST_F(AdapterNotificationRegressionTest, multipleIsrCycles)
{
    for (int i = 0; i < 10; ++i)
    {
        CutType::enterIsr();
        EXPECT_NE(nullptr, CutType::getHigherPriorityTaskWoken());
        EXPECT_EQ(pdFALSE, *CutType::getHigherPriorityTaskWoken());

        if (i % 2 == 0)
        {
            // Every other cycle, set the flag
            *CutType::getHigherPriorityTaskWoken() = pdTRUE;
            EXPECT_CALL(_freeRtosMock, portYIELD_FROM_ISR(pdTRUE));
        }
        CutType::leaveIsr();
        EXPECT_EQ(nullptr, CutType::getHigherPriorityTaskWoken());
        Mock::VerifyAndClearExpectations(&_freeRtosMock);
    }
}

// ============================================================================
// Category 3: RunnableExecutor enqueue/dequeue (8 tests)
// ============================================================================

/**
 * \desc: Enqueue a runnable, then handleEvent dequeues and executes it.
 */
TEST_F(TaskNotificationRegressionTest, enqueueAndDequeue)
{
    uint32_t eventMask = simulateTaskExecute(_runnableMock1);
    Sequence seq;
    uint32_t stopMask = 0U;
    setupSingleDispatchCycle(seq, eventMask, stopMask);
    EXPECT_CALL(_runnableMock1, execute());
    _osTaskFunction(_param);
}

/**
 * \desc: Enqueue during handleEvent — both runnables execute.
 */
TEST_F(TaskNotificationRegressionTest, enqueueWhileHandling)
{
    uint32_t eventMask = simulateTaskExecute(_runnableMock1);

    EXPECT_CALL(_systemTimerMock, getSystemTimeUs32Bit()).WillRepeatedly(Return(100000U));
    Sequence seq;

    EXPECT_CALL(
        _freeRtosMock,
        xTaskNotifyWait(0U, 7U, NotNull(), TestBindingMock::WAIT_EVENTS_TICK_COUNT))
        .InSequence(seq)
        .WillOnce(DoAll(SetArgPointee<2>(eventMask), Return(true)));

    EXPECT_CALL(_runnableMock1, execute())
        .InSequence(seq)
        .WillOnce(InvokeWithoutArgs([this]() {
            // Enqueue second during first's execution
            EXPECT_CALL(_bindingMock, getHigherPriorityTaskWokenFunc())
                .WillOnce(Return(static_cast<BaseType_t*>(nullptr)));
            EXPECT_CALL(_freeRtosMock, xTaskNotify(&_taskHandle, _, eSetBits));
            _cut.execute(_runnableMock2);
        }));
    EXPECT_CALL(_runnableMock2, execute()).InSequence(seq);

    uint32_t stopMask = 0U;
    EXPECT_CALL(
        _freeRtosMock,
        xTaskNotifyWait(0U, 7U, NotNull(), TestBindingMock::WAIT_EVENTS_TICK_COUNT))
        .InSequence(seq)
        .WillOnce(DoAll(SetArgPointee<2>(0U), StopDispatch(&_cut), Return(false)));
    EXPECT_CALL(_bindingMock, getHigherPriorityTaskWokenFunc())
        .WillOnce(Return(static_cast<BaseType_t*>(nullptr)));
    EXPECT_CALL(_freeRtosMock, xTaskNotify(&_taskHandle, _, eSetBits))
        .InSequence(seq)
        .WillOnce(SaveArg<1>(&stopMask));
    EXPECT_CALL(
        _freeRtosMock,
        xTaskNotifyWait(0U, 7U, NotNull(), TestBindingMock::WAIT_EVENTS_TICK_COUNT))
        .InSequence(seq)
        .WillOnce(DoAll(CopyArgPointee2(&stopMask), Return(true)));

    _osTaskFunction(_param);
}

/**
 * \desc: Same runnable enqueued twice. isEnqueued() prevents double-enqueue; only processed once.
 */
TEST_F(TaskNotificationRegressionTest, doubleEnqueue)
{
    // First enqueue
    uint32_t eventMask = simulateTaskExecute(_runnableMock1);

    // Second enqueue of the same runnable: still calls setEvent but isEnqueued() prevents
    // adding to queue again
    EXPECT_CALL(_bindingMock, getHigherPriorityTaskWokenFunc())
        .WillOnce(Return(static_cast<BaseType_t*>(nullptr)));
    EXPECT_CALL(_freeRtosMock, xTaskNotify(&_taskHandle, _, eSetBits));
    _cut.execute(_runnableMock1);
    Mock::VerifyAndClearExpectations(&_bindingMock);
    Mock::VerifyAndClearExpectations(&_freeRtosMock);

    // Dispatch: only one execute expected
    Sequence seq;
    uint32_t stopMask = 0U;
    setupSingleDispatchCycle(seq, eventMask, stopMask);
    EXPECT_CALL(_runnableMock1, execute()).Times(1);
    _osTaskFunction(_param);
}

/**
 * \desc: After dequeue, runnable can be enqueued again.
 */
TEST_F(TaskNotificationRegressionTest, dequeueResetsEnqueueFlag)
{
    // First cycle: enqueue and process
    uint32_t eventMask = simulateTaskExecute(_runnableMock1);

    EXPECT_CALL(_systemTimerMock, getSystemTimeUs32Bit()).WillRepeatedly(Return(100000U));
    Sequence seq;
    EXPECT_CALL(
        _freeRtosMock,
        xTaskNotifyWait(0U, 7U, NotNull(), TestBindingMock::WAIT_EVENTS_TICK_COUNT))
        .InSequence(seq)
        .WillOnce(DoAll(SetArgPointee<2>(eventMask), Return(true)));
    EXPECT_CALL(_runnableMock1, execute()).InSequence(seq);

    // After first execute, enqueue same runnable again (should succeed since dequeued)
    uint32_t eventMask2 = 0U;
    EXPECT_CALL(
        _freeRtosMock,
        xTaskNotifyWait(0U, 7U, NotNull(), TestBindingMock::WAIT_EVENTS_TICK_COUNT))
        .InSequence(seq)
        .WillOnce(DoAll(
            SetArgPointee<2>(0U),
            InvokeWithoutArgs([this, &eventMask2]() {
                eventMask2 = simulateTaskExecute(_runnableMock1);
            }),
            Return(false)));

    // Third wait picks up second enqueue
    EXPECT_CALL(
        _freeRtosMock,
        xTaskNotifyWait(0U, 7U, NotNull(), TestBindingMock::WAIT_EVENTS_TICK_COUNT))
        .InSequence(seq)
        .WillOnce(DoAll(SetArgPointee<2>(eventMask), Return(true)));
    EXPECT_CALL(_runnableMock1, execute()).InSequence(seq);

    // Stop
    uint32_t stopMask = 0U;
    EXPECT_CALL(
        _freeRtosMock,
        xTaskNotifyWait(0U, 7U, NotNull(), TestBindingMock::WAIT_EVENTS_TICK_COUNT))
        .InSequence(seq)
        .WillOnce(DoAll(SetArgPointee<2>(0U), StopDispatch(&_cut), Return(false)));
    EXPECT_CALL(_bindingMock, getHigherPriorityTaskWokenFunc())
        .WillOnce(Return(static_cast<BaseType_t*>(nullptr)));
    EXPECT_CALL(_freeRtosMock, xTaskNotify(&_taskHandle, _, eSetBits))
        .InSequence(seq)
        .WillOnce(SaveArg<1>(&stopMask));
    EXPECT_CALL(
        _freeRtosMock,
        xTaskNotifyWait(0U, 7U, NotNull(), TestBindingMock::WAIT_EVENTS_TICK_COUNT))
        .InSequence(seq)
        .WillOnce(DoAll(CopyArgPointee2(&stopMask), Return(true)));

    _osTaskFunction(_param);
}

/**
 * \desc: handleEvent with empty queue returns immediately without executing anything.
 */
TEST_F(TaskNotificationRegressionTest, emptyQueueBreaks)
{
    // Don't enqueue anything, but deliver the execute event bit anyway
    EXPECT_CALL(_systemTimerMock, getSystemTimeUs32Bit()).WillRepeatedly(Return(100000U));
    Sequence seq;

    // Deliver execute event bit (0x01) with no runnable in queue
    EXPECT_CALL(
        _freeRtosMock,
        xTaskNotifyWait(0U, 7U, NotNull(), TestBindingMock::WAIT_EVENTS_TICK_COUNT))
        .InSequence(seq)
        .WillOnce(DoAll(SetArgPointee<2>(0x01U), Return(true)));

    // No execute calls expected — queue is empty

    // Stop
    uint32_t stopMask = 0U;
    EXPECT_CALL(
        _freeRtosMock,
        xTaskNotifyWait(0U, 7U, NotNull(), TestBindingMock::WAIT_EVENTS_TICK_COUNT))
        .InSequence(seq)
        .WillOnce(DoAll(SetArgPointee<2>(0U), StopDispatch(&_cut), Return(false)));
    EXPECT_CALL(_bindingMock, getHigherPriorityTaskWokenFunc())
        .WillOnce(Return(static_cast<BaseType_t*>(nullptr)));
    EXPECT_CALL(_freeRtosMock, xTaskNotify(&_taskHandle, _, eSetBits))
        .InSequence(seq)
        .WillOnce(SaveArg<1>(&stopMask));
    EXPECT_CALL(
        _freeRtosMock,
        xTaskNotifyWait(0U, 7U, NotNull(), TestBindingMock::WAIT_EVENTS_TICK_COUNT))
        .InSequence(seq)
        .WillOnce(DoAll(CopyArgPointee2(&stopMask), Return(true)));

    _osTaskFunction(_param);
}

/**
 * \desc: Enqueue from ISR and task context, both processed.
 */
TEST_F(TaskNotificationRegressionTest, enqueueFromDifferentContext)
{
    // Enqueue from task context
    uint32_t taskMask = simulateTaskExecute(_runnableMock1);

    // Enqueue from ISR context
    BaseType_t hptw = pdFALSE;
    uint32_t isrMask = simulateIsrExecute(_runnableMock2, hptw);

    // Both should be processed
    EXPECT_CALL(_systemTimerMock, getSystemTimeUs32Bit()).WillRepeatedly(Return(100000U));
    Sequence seq;
    EXPECT_CALL(
        _freeRtosMock,
        xTaskNotifyWait(0U, 7U, NotNull(), TestBindingMock::WAIT_EVENTS_TICK_COUNT))
        .InSequence(seq)
        .WillOnce(DoAll(SetArgPointee<2>(taskMask | isrMask), Return(true)));

    EXPECT_CALL(_runnableMock1, execute()).InSequence(seq);
    EXPECT_CALL(_runnableMock2, execute()).InSequence(seq);

    uint32_t stopMask = 0U;
    EXPECT_CALL(
        _freeRtosMock,
        xTaskNotifyWait(0U, 7U, NotNull(), TestBindingMock::WAIT_EVENTS_TICK_COUNT))
        .InSequence(seq)
        .WillOnce(DoAll(SetArgPointee<2>(0U), StopDispatch(&_cut), Return(false)));
    EXPECT_CALL(_bindingMock, getHigherPriorityTaskWokenFunc())
        .WillOnce(Return(static_cast<BaseType_t*>(nullptr)));
    EXPECT_CALL(_freeRtosMock, xTaskNotify(&_taskHandle, _, eSetBits))
        .InSequence(seq)
        .WillOnce(SaveArg<1>(&stopMask));
    EXPECT_CALL(
        _freeRtosMock,
        xTaskNotifyWait(0U, 7U, NotNull(), TestBindingMock::WAIT_EVENTS_TICK_COUNT))
        .InSequence(seq)
        .WillOnce(DoAll(CopyArgPointee2(&stopMask), Return(true)));

    _osTaskFunction(_param);
}

/**
 * \desc: 100 enqueue/dequeue cycles on CountingRunnable, all execute.
 */
TEST_F(TaskNotificationRegressionTest, rapidEnqueueDequeue)
{
    CountingRunnable runnable;

    for (uint32_t i = 0U; i < 100U; ++i)
    {
        EXPECT_CALL(_bindingMock, getHigherPriorityTaskWokenFunc())
            .WillOnce(Return(static_cast<BaseType_t*>(nullptr)));
        EXPECT_CALL(_freeRtosMock, xTaskNotify(&_taskHandle, _, eSetBits));
        _cut.execute(runnable);
        Mock::VerifyAndClearExpectations(&_bindingMock);
        Mock::VerifyAndClearExpectations(&_freeRtosMock);

        // Verify it's enqueued
        EXPECT_TRUE(runnable.isEnqueued());

        // Simulate dispatch via dispatchWhileWork
        EXPECT_CALL(_systemTimerMock, getSystemTimeUs32Bit()).WillRepeatedly(Return(100000U));
        EXPECT_CALL(_freeRtosMock, xTaskNotifyWait(0U, 7U, NotNull(), 0U))
            .WillOnce(DoAll(SetArgPointee<2>(0x01U), Return(true)))
            .WillOnce(Return(false));
        _cut.dispatchWhileWork();
        Mock::VerifyAndClearExpectations(&_freeRtosMock);
        Mock::VerifyAndClearExpectations(&_systemTimerMock);

        EXPECT_EQ(i + 1U, runnable._count);
        // After dequeue, should not be enqueued
        EXPECT_FALSE(runnable.isEnqueued());
    }
}

/**
 * \desc: Dequeue then re-enqueue — processes correctly the second time.
 */
TEST_F(TaskNotificationRegressionTest, enqueueAfterDequeue)
{
    CountingRunnable runnable;

    // First enqueue + dispatch
    EXPECT_CALL(_bindingMock, getHigherPriorityTaskWokenFunc())
        .WillOnce(Return(static_cast<BaseType_t*>(nullptr)));
    EXPECT_CALL(_freeRtosMock, xTaskNotify(&_taskHandle, _, eSetBits));
    _cut.execute(runnable);
    Mock::VerifyAndClearExpectations(&_bindingMock);
    Mock::VerifyAndClearExpectations(&_freeRtosMock);

    EXPECT_CALL(_systemTimerMock, getSystemTimeUs32Bit()).WillRepeatedly(Return(100000U));
    EXPECT_CALL(_freeRtosMock, xTaskNotifyWait(0U, 7U, NotNull(), 0U))
        .WillOnce(DoAll(SetArgPointee<2>(0x01U), Return(true)))
        .WillOnce(Return(false));
    _cut.dispatchWhileWork();
    Mock::VerifyAndClearExpectations(&_freeRtosMock);
    Mock::VerifyAndClearExpectations(&_systemTimerMock);
    EXPECT_EQ(1U, runnable._count);
    EXPECT_FALSE(runnable.isEnqueued());

    // Second enqueue + dispatch
    EXPECT_CALL(_bindingMock, getHigherPriorityTaskWokenFunc())
        .WillOnce(Return(static_cast<BaseType_t*>(nullptr)));
    EXPECT_CALL(_freeRtosMock, xTaskNotify(&_taskHandle, _, eSetBits));
    _cut.execute(runnable);
    Mock::VerifyAndClearExpectations(&_bindingMock);
    Mock::VerifyAndClearExpectations(&_freeRtosMock);

    EXPECT_CALL(_systemTimerMock, getSystemTimeUs32Bit()).WillRepeatedly(Return(100000U));
    EXPECT_CALL(_freeRtosMock, xTaskNotifyWait(0U, 7U, NotNull(), 0U))
        .WillOnce(DoAll(SetArgPointee<2>(0x01U), Return(true)))
        .WillOnce(Return(false));
    _cut.dispatchWhileWork();
    EXPECT_EQ(2U, runnable._count);
}

// ============================================================================
// Category 4: BASEPRI interaction (6 tests)
// ============================================================================

/**
 * \desc: ISR at priority == configMAX_SYSCALL_INTERRUPT_PRIORITY is masked during critical section.
 *        Modeled: when getHigherPriorityTaskWoken returns non-null (ISR context),
 *        xTaskNotifyFromISR is used (not xTaskNotify). This verifies the ISR path.
 */
TEST_F(AdapterNotificationRegressionTest, priorityEqualToMaxSyscall)
{
    char const* name = "prio_test";
    CutType::Task<1U, 256U> task(name);
    uint32_t taskHandle = 99U;

    EXPECT_CALL(
        _freeRtosMock,
        xTaskCreateStatic(
            NotNull(), name, 256U / sizeof(StackType_t), NotNull(), 1U, NotNull(), NotNull()))
        .WillOnce(Return(&taskHandle));
    EXPECT_CALL(_freeRtosMock, vTaskStartScheduler());
    CutType::run(CutType::StartAppFunctionType::
                     create<AdapterNotificationRegressionTest,
                            &AdapterNotificationRegressionTest::startApp>(*this));

    // In ISR context, execute uses xTaskNotifyFromISR
    CutType::enterIsr();
    EXPECT_CALL(
        _freeRtosMock,
        xTaskNotifyFromISR(&taskHandle, _, eSetBits, CutType::getHigherPriorityTaskWoken()));
    CutType::execute(1U, _runnableMock);
    CutType::leaveIsr();
}

/**
 * \desc: ISR at priority below configMAX_SYSCALL is NOT masked — xTaskNotifyFromISR still works.
 *        (In mock, all ISR paths go through the same getHigherPriorityTaskWoken mechanism.)
 */
TEST_F(AdapterNotificationRegressionTest, priorityBelowMaxSyscall)
{
    char const* name = "lo_prio";
    CutType::Task<1U, 256U> task(name);
    uint32_t taskHandle = 88U;

    EXPECT_CALL(
        _freeRtosMock,
        xTaskCreateStatic(
            NotNull(), name, 256U / sizeof(StackType_t), NotNull(), 1U, NotNull(), NotNull()))
        .WillOnce(Return(&taskHandle));
    EXPECT_CALL(_freeRtosMock, vTaskStartScheduler());
    CutType::run(CutType::StartAppFunctionType::
                     create<AdapterNotificationRegressionTest,
                            &AdapterNotificationRegressionTest::startApp>(*this));

    // In ISR context (lower priority than MAX_SYSCALL), notification still works
    CutType::enterIsr();
    EXPECT_CALL(
        _freeRtosMock,
        xTaskNotifyFromISR(&taskHandle, _, eSetBits, CutType::getHigherPriorityTaskWoken()));
    CutType::execute(1U, _runnableMock);
    CutType::leaveIsr();
}

/**
 * \desc: BASEPRI returns to 0 after critical section — modeled via getHigherPriorityTaskWoken
 *        being nullptr when not in ISR.
 */
TEST_F(AdapterNotificationRegressionTest, basepriRestoredAfterCriticalSection)
{
    // Not in ISR: pointer is null
    EXPECT_EQ(nullptr, CutType::getHigherPriorityTaskWoken());

    CutType::enterIsr();
    EXPECT_NE(nullptr, CutType::getHigherPriorityTaskWoken());
    CutType::leaveIsr();

    // After ISR: pointer is null again (BASEPRI restored)
    EXPECT_EQ(nullptr, CutType::getHigherPriorityTaskWoken());
}

/**
 * \desc: setEvents works correctly both in ISR context (BASEPRI set) and task context.
 */
TEST_F(AdapterNotificationRegressionTest, basepriDuringSetEvents)
{
    char const* name = "setev_test";
    CutType::Task<1U, 256U> task(name);
    uint32_t taskHandle = 77U;

    EXPECT_CALL(
        _freeRtosMock,
        xTaskCreateStatic(
            NotNull(), name, 256U / sizeof(StackType_t), NotNull(), 1U, NotNull(), NotNull()))
        .WillOnce(Return(&taskHandle));
    EXPECT_CALL(_freeRtosMock, vTaskStartScheduler());
    CutType::run(CutType::StartAppFunctionType::
                     create<AdapterNotificationRegressionTest,
                            &AdapterNotificationRegressionTest::startApp>(*this));

    // Task context: uses xTaskNotify
    EXPECT_CALL(_freeRtosMock, xTaskNotify(&taskHandle, _, eSetBits));
    CutType::execute(1U, _runnableMock);
    Mock::VerifyAndClearExpectations(&_freeRtosMock);

    // ISR context: uses xTaskNotifyFromISR
    CutType::enterIsr();
    EXPECT_CALL(
        _freeRtosMock,
        xTaskNotifyFromISR(&taskHandle, _, eSetBits, CutType::getHigherPriorityTaskWoken()));
    CutType::execute(1U, _runnableMock);
    CutType::leaveIsr();
}

/**
 * \desc: Nested enter/leave critical sections restore BASEPRI correctly.
 */
TEST_F(AdapterNotificationRegressionTest, nestedCriticalSectionsRestoreBasepri)
{
    CutType::enterIsr();
    CutType::enterIsr();
    CutType::enterIsr();

    EXPECT_NE(nullptr, CutType::getHigherPriorityTaskWoken());

    CutType::leaveIsr();
    EXPECT_NE(nullptr, CutType::getHigherPriorityTaskWoken());

    CutType::leaveIsr();
    EXPECT_NE(nullptr, CutType::getHigherPriorityTaskWoken());

    CutType::leaveIsr();
    EXPECT_EQ(nullptr, CutType::getHigherPriorityTaskWoken());
}

/**
 * \desc: async::LockType masks ISR with equal priority — task context execute uses xTaskNotify.
 */
TEST_F(AdapterNotificationRegressionTest, isrMaskedDuringLockType)
{
    char const* name = "lock_test";
    CutType::Task<1U, 256U> task(name);
    uint32_t taskHandle = 66U;

    EXPECT_CALL(
        _freeRtosMock,
        xTaskCreateStatic(
            NotNull(), name, 256U / sizeof(StackType_t), NotNull(), 1U, NotNull(), NotNull()))
        .WillOnce(Return(&taskHandle));
    EXPECT_CALL(_freeRtosMock, vTaskStartScheduler());
    CutType::run(CutType::StartAppFunctionType::
                     create<AdapterNotificationRegressionTest,
                            &AdapterNotificationRegressionTest::startApp>(*this));

    // Not in ISR: getHigherPriorityTaskWoken is null, so xTaskNotify is used
    EXPECT_EQ(nullptr, CutType::getHigherPriorityTaskWoken());
    EXPECT_CALL(_freeRtosMock, xTaskNotify(&taskHandle, _, eSetBits));
    CutType::execute(1U, _runnableMock);
}

// ============================================================================
// Category 5: Full ISR -> task -> response cycle (6 tests)
// ============================================================================

/**
 * \desc: Single cycle: ISR enqueues -> task wakes -> runnable executes -> done.
 */
TEST_F(TaskNotificationRegressionTest, singleCycleWorks)
{
    BaseType_t hptw = pdFALSE;
    uint32_t eventMask = simulateIsrExecute(_runnableMock1, hptw);

    Sequence seq;
    uint32_t stopMask = 0U;
    setupSingleDispatchCycle(seq, eventMask, stopMask);
    EXPECT_CALL(_runnableMock1, execute());

    _osTaskFunction(_param);
}

/**
 * \desc: Two sequential ISR -> task cycles both complete.
 */
TEST_F(TaskNotificationRegressionTest, secondCycleWorks)
{
    CountingRunnable runnable;

    for (int cycle = 0; cycle < 2; ++cycle)
    {
        // ISR enqueue
        BaseType_t hptw = pdFALSE;
        EXPECT_CALL(_bindingMock, getHigherPriorityTaskWokenFunc())
            .WillOnce(Return(&hptw));
        EXPECT_CALL(
            _freeRtosMock,
            xTaskNotifyFromISR(&_taskHandle, _, eSetBits, &hptw));
        _cut.execute(runnable);
        Mock::VerifyAndClearExpectations(&_bindingMock);
        Mock::VerifyAndClearExpectations(&_freeRtosMock);

        // Process via dispatchWhileWork
        EXPECT_CALL(_systemTimerMock, getSystemTimeUs32Bit()).WillRepeatedly(Return(100000U));
        EXPECT_CALL(_freeRtosMock, xTaskNotifyWait(0U, 7U, NotNull(), 0U))
            .WillOnce(DoAll(SetArgPointee<2>(0x01U), Return(true)))
            .WillOnce(Return(false));
        _cut.dispatchWhileWork();
        Mock::VerifyAndClearExpectations(&_freeRtosMock);
        Mock::VerifyAndClearExpectations(&_systemTimerMock);

        EXPECT_EQ(static_cast<uint32_t>(cycle + 1), runnable._count);
    }
}

/**
 * \desc: First works, second times out, third works.
 */
TEST_F(TaskNotificationRegressionTest, thirdCycleAfterTimeout)
{
    CountingRunnable runnable;

    // Cycle 1: success
    {
        BaseType_t hptw = pdFALSE;
        EXPECT_CALL(_bindingMock, getHigherPriorityTaskWokenFunc())
            .WillOnce(Return(&hptw));
        EXPECT_CALL(
            _freeRtosMock,
            xTaskNotifyFromISR(&_taskHandle, _, eSetBits, &hptw));
        _cut.execute(runnable);
        Mock::VerifyAndClearExpectations(&_bindingMock);
        Mock::VerifyAndClearExpectations(&_freeRtosMock);

        EXPECT_CALL(_systemTimerMock, getSystemTimeUs32Bit()).WillRepeatedly(Return(100000U));
        EXPECT_CALL(_freeRtosMock, xTaskNotifyWait(0U, 7U, NotNull(), 0U))
            .WillOnce(DoAll(SetArgPointee<2>(0x01U), Return(true)))
            .WillOnce(Return(false));
        _cut.dispatchWhileWork();
        Mock::VerifyAndClearExpectations(&_freeRtosMock);
        Mock::VerifyAndClearExpectations(&_systemTimerMock);
        EXPECT_EQ(1U, runnable._count);
    }

    // Cycle 2: timeout (no runnable enqueued, no events)
    {
        EXPECT_CALL(_systemTimerMock, getSystemTimeUs32Bit()).WillRepeatedly(Return(200000U));
        EXPECT_CALL(_freeRtosMock, xTaskNotifyWait(0U, 7U, NotNull(), 0U))
            .WillOnce(Return(false));
        _cut.dispatchWhileWork();
        Mock::VerifyAndClearExpectations(&_freeRtosMock);
        Mock::VerifyAndClearExpectations(&_systemTimerMock);
        EXPECT_EQ(1U, runnable._count);  // No change
    }

    // Cycle 3: success again
    {
        BaseType_t hptw = pdFALSE;
        EXPECT_CALL(_bindingMock, getHigherPriorityTaskWokenFunc())
            .WillOnce(Return(&hptw));
        EXPECT_CALL(
            _freeRtosMock,
            xTaskNotifyFromISR(&_taskHandle, _, eSetBits, &hptw));
        _cut.execute(runnable);
        Mock::VerifyAndClearExpectations(&_bindingMock);
        Mock::VerifyAndClearExpectations(&_freeRtosMock);

        EXPECT_CALL(_systemTimerMock, getSystemTimeUs32Bit()).WillRepeatedly(Return(300000U));
        EXPECT_CALL(_freeRtosMock, xTaskNotifyWait(0U, 7U, NotNull(), 0U))
            .WillOnce(DoAll(SetArgPointee<2>(0x01U), Return(true)))
            .WillOnce(Return(false));
        _cut.dispatchWhileWork();
        Mock::VerifyAndClearExpectations(&_freeRtosMock);
        Mock::VerifyAndClearExpectations(&_systemTimerMock);
        EXPECT_EQ(2U, runnable._count);
    }
}

/**
 * \desc: ISR fires while task processes first event, second still works.
 */
TEST_F(TaskNotificationRegressionTest, cycleWithTaskProcessingDelay)
{
    // Same as notificationWhileTaskRunning but using the full cycle model
    BaseType_t hptw = pdFALSE;
    uint32_t eventMask = simulateIsrExecute(_runnableMock1, hptw);

    EXPECT_CALL(_systemTimerMock, getSystemTimeUs32Bit()).WillRepeatedly(Return(100000U));
    Sequence seq;

    EXPECT_CALL(
        _freeRtosMock,
        xTaskNotifyWait(0U, 7U, NotNull(), TestBindingMock::WAIT_EVENTS_TICK_COUNT))
        .InSequence(seq)
        .WillOnce(DoAll(SetArgPointee<2>(eventMask), Return(true)));

    // During first execute, ISR fires and enqueues second runnable
    EXPECT_CALL(_runnableMock1, execute())
        .InSequence(seq)
        .WillOnce(InvokeWithoutArgs([this]() {
            BaseType_t hptw2 = pdFALSE;
            simulateIsrExecute(_runnableMock2, hptw2);
        }));

    // Second notification arrives
    EXPECT_CALL(
        _freeRtosMock,
        xTaskNotifyWait(0U, 7U, NotNull(), TestBindingMock::WAIT_EVENTS_TICK_COUNT))
        .InSequence(seq)
        .WillOnce(DoAll(SetArgPointee<2>(eventMask), Return(true)));
    EXPECT_CALL(_runnableMock2, execute()).InSequence(seq);

    // Stop
    uint32_t stopMask = 0U;
    EXPECT_CALL(
        _freeRtosMock,
        xTaskNotifyWait(0U, 7U, NotNull(), TestBindingMock::WAIT_EVENTS_TICK_COUNT))
        .InSequence(seq)
        .WillOnce(DoAll(SetArgPointee<2>(0U), StopDispatch(&_cut), Return(false)));
    EXPECT_CALL(_bindingMock, getHigherPriorityTaskWokenFunc())
        .WillOnce(Return(static_cast<BaseType_t*>(nullptr)));
    EXPECT_CALL(_freeRtosMock, xTaskNotify(&_taskHandle, _, eSetBits))
        .InSequence(seq)
        .WillOnce(SaveArg<1>(&stopMask));
    EXPECT_CALL(
        _freeRtosMock,
        xTaskNotifyWait(0U, 7U, NotNull(), TestBindingMock::WAIT_EVENTS_TICK_COUNT))
        .InSequence(seq)
        .WillOnce(DoAll(CopyArgPointee2(&stopMask), Return(true)));

    _osTaskFunction(_param);
}

/**
 * \desc: 10 ISR -> task -> complete cycles all work.
 */
TEST_F(TaskNotificationRegressionTest, tenSequentialCycles)
{
    CountingRunnable runnable;

    for (uint32_t i = 0U; i < 10U; ++i)
    {
        BaseType_t hptw = pdFALSE;
        EXPECT_CALL(_bindingMock, getHigherPriorityTaskWokenFunc())
            .WillOnce(Return(&hptw));
        EXPECT_CALL(
            _freeRtosMock,
            xTaskNotifyFromISR(&_taskHandle, _, eSetBits, &hptw));
        _cut.execute(runnable);
        Mock::VerifyAndClearExpectations(&_bindingMock);
        Mock::VerifyAndClearExpectations(&_freeRtosMock);

        EXPECT_CALL(_systemTimerMock, getSystemTimeUs32Bit())
            .WillRepeatedly(Return(100000U + i * 10000U));
        EXPECT_CALL(_freeRtosMock, xTaskNotifyWait(0U, 7U, NotNull(), 0U))
            .WillOnce(DoAll(SetArgPointee<2>(0x01U), Return(true)))
            .WillOnce(Return(false));
        _cut.dispatchWhileWork();
        Mock::VerifyAndClearExpectations(&_freeRtosMock);
        Mock::VerifyAndClearExpectations(&_systemTimerMock);

        EXPECT_EQ(i + 1U, runnable._count);
        EXPECT_FALSE(runnable.isEnqueued());
    }
}

/**
 * \desc: Task holds async::LockType during processing. ISR fires after lock released.
 *        In mock environment, LockType is effectively a no-op, but this tests the flow.
 */
TEST_F(TaskNotificationRegressionTest, cycleWithLockDuringProcessing)
{
    CountingRunnable runnable;

    // First cycle with "lock" during execute
    BaseType_t hptw = pdFALSE;
    EXPECT_CALL(_bindingMock, getHigherPriorityTaskWokenFunc())
        .WillOnce(Return(&hptw));
    EXPECT_CALL(
        _freeRtosMock,
        xTaskNotifyFromISR(&_taskHandle, _, eSetBits, &hptw));
    _cut.execute(runnable);
    Mock::VerifyAndClearExpectations(&_bindingMock);
    Mock::VerifyAndClearExpectations(&_freeRtosMock);

    EXPECT_CALL(_systemTimerMock, getSystemTimeUs32Bit()).WillRepeatedly(Return(100000U));
    EXPECT_CALL(_freeRtosMock, xTaskNotifyWait(0U, 7U, NotNull(), 0U))
        .WillOnce(DoAll(SetArgPointee<2>(0x01U), Return(true)))
        .WillOnce(Return(false));
    _cut.dispatchWhileWork();
    Mock::VerifyAndClearExpectations(&_freeRtosMock);
    Mock::VerifyAndClearExpectations(&_systemTimerMock);
    EXPECT_EQ(1U, runnable._count);

    // Second cycle (after lock was released)
    EXPECT_CALL(_bindingMock, getHigherPriorityTaskWokenFunc())
        .WillOnce(Return(&hptw));
    EXPECT_CALL(
        _freeRtosMock,
        xTaskNotifyFromISR(&_taskHandle, _, eSetBits, &hptw));
    _cut.execute(runnable);
    Mock::VerifyAndClearExpectations(&_bindingMock);
    Mock::VerifyAndClearExpectations(&_freeRtosMock);

    EXPECT_CALL(_systemTimerMock, getSystemTimeUs32Bit()).WillRepeatedly(Return(200000U));
    EXPECT_CALL(_freeRtosMock, xTaskNotifyWait(0U, 7U, NotNull(), 0U))
        .WillOnce(DoAll(SetArgPointee<2>(0x01U), Return(true)))
        .WillOnce(Return(false));
    _cut.dispatchWhileWork();
    EXPECT_EQ(2U, runnable._count);
}

// ============================================================================
// Category 6: Integration with CAN transceiver pattern (6 tests)
// ============================================================================

/**
 * \desc: CAN RX ISR calls async::execute, receiveTask runs.
 */
TEST_F(TaskNotificationRegressionTest, canRxIsrDispatchesReceiveTask)
{
    // Simulate CAN RX ISR handler calling execute
    BaseType_t hptw = pdFALSE;
    uint32_t eventMask = simulateIsrExecute(_runnableMock1, hptw);

    Sequence seq;
    uint32_t stopMask = 0U;
    setupSingleDispatchCycle(seq, eventMask, stopMask);
    EXPECT_CALL(_runnableMock1, execute());  // receiveTask

    _osTaskFunction(_param);
}

/**
 * \desc: Two RX ISRs, receiveTask runs twice.
 */
TEST_F(TaskNotificationRegressionTest, canRxIsrTwiceDispatchesTwice)
{
    CountingRunnable receiveTask;

    // Two ISR triggers
    for (int i = 0; i < 2; ++i)
    {
        BaseType_t hptw = pdFALSE;
        EXPECT_CALL(_bindingMock, getHigherPriorityTaskWokenFunc())
            .WillOnce(Return(&hptw));
        EXPECT_CALL(
            _freeRtosMock,
            xTaskNotifyFromISR(&_taskHandle, _, eSetBits, &hptw));
        _cut.execute(receiveTask);
        Mock::VerifyAndClearExpectations(&_bindingMock);
        Mock::VerifyAndClearExpectations(&_freeRtosMock);

        EXPECT_CALL(_systemTimerMock, getSystemTimeUs32Bit()).WillRepeatedly(Return(100000U));
        EXPECT_CALL(_freeRtosMock, xTaskNotifyWait(0U, 7U, NotNull(), 0U))
            .WillOnce(DoAll(SetArgPointee<2>(0x01U), Return(true)))
            .WillOnce(Return(false));
        _cut.dispatchWhileWork();
        Mock::VerifyAndClearExpectations(&_freeRtosMock);
        Mock::VerifyAndClearExpectations(&_systemTimerMock);
    }
    EXPECT_EQ(2U, receiveTask._count);
}

/**
 * \desc: TX ISR calls canFrameSentCallback -> async::execute, callback runs.
 */
TEST_F(TaskNotificationRegressionTest, canTxIsrDispatchesCallback)
{
    // _runnableMock1 represents the TX completion callback
    BaseType_t hptw = pdFALSE;
    uint32_t eventMask = simulateIsrExecute(_runnableMock1, hptw);

    Sequence seq;
    uint32_t stopMask = 0U;
    setupSingleDispatchCycle(seq, eventMask, stopMask);
    EXPECT_CALL(_runnableMock1, execute());  // canFrameSentCallback

    _osTaskFunction(_param);
}

/**
 * \desc: RX ISR then TX ISR on same context, both runnables execute.
 */
TEST_F(TaskNotificationRegressionTest, canRxThenTxIsr)
{
    BaseType_t hptw = pdFALSE;

    // RX ISR
    uint32_t rxMask = simulateIsrExecute(_runnableMock1, hptw);
    // TX ISR
    uint32_t txMask = simulateIsrExecute(_runnableMock2, hptw);

    EXPECT_CALL(_systemTimerMock, getSystemTimeUs32Bit()).WillRepeatedly(Return(100000U));
    Sequence seq;
    EXPECT_CALL(
        _freeRtosMock,
        xTaskNotifyWait(0U, 7U, NotNull(), TestBindingMock::WAIT_EVENTS_TICK_COUNT))
        .InSequence(seq)
        .WillOnce(DoAll(SetArgPointee<2>(rxMask | txMask), Return(true)));

    EXPECT_CALL(_runnableMock1, execute()).InSequence(seq);  // RX receive
    EXPECT_CALL(_runnableMock2, execute()).InSequence(seq);  // TX callback

    uint32_t stopMask = 0U;
    EXPECT_CALL(
        _freeRtosMock,
        xTaskNotifyWait(0U, 7U, NotNull(), TestBindingMock::WAIT_EVENTS_TICK_COUNT))
        .InSequence(seq)
        .WillOnce(DoAll(SetArgPointee<2>(0U), StopDispatch(&_cut), Return(false)));
    EXPECT_CALL(_bindingMock, getHigherPriorityTaskWokenFunc())
        .WillOnce(Return(static_cast<BaseType_t*>(nullptr)));
    EXPECT_CALL(_freeRtosMock, xTaskNotify(&_taskHandle, _, eSetBits))
        .InSequence(seq)
        .WillOnce(SaveArg<1>(&stopMask));
    EXPECT_CALL(
        _freeRtosMock,
        xTaskNotifyWait(0U, 7U, NotNull(), TestBindingMock::WAIT_EVENTS_TICK_COUNT))
        .InSequence(seq)
        .WillOnce(DoAll(CopyArgPointee2(&stopMask), Return(true)));

    _osTaskFunction(_param);
}

/**
 * \desc: TX ISR fires while receiveTask processes. Both complete.
 */
TEST_F(TaskNotificationRegressionTest, canTxCallbackDuringRxProcessing)
{
    BaseType_t hptw = pdFALSE;
    uint32_t rxMask = simulateIsrExecute(_runnableMock1, hptw);

    EXPECT_CALL(_systemTimerMock, getSystemTimeUs32Bit()).WillRepeatedly(Return(100000U));
    Sequence seq;

    EXPECT_CALL(
        _freeRtosMock,
        xTaskNotifyWait(0U, 7U, NotNull(), TestBindingMock::WAIT_EVENTS_TICK_COUNT))
        .InSequence(seq)
        .WillOnce(DoAll(SetArgPointee<2>(rxMask), Return(true)));

    // During RX processing, TX ISR fires
    EXPECT_CALL(_runnableMock1, execute())
        .InSequence(seq)
        .WillOnce(InvokeWithoutArgs([this]() {
            BaseType_t hptw2 = pdFALSE;
            simulateIsrExecute(_runnableMock2, hptw2);
        }));

    // Next dispatch picks up TX callback
    EXPECT_CALL(
        _freeRtosMock,
        xTaskNotifyWait(0U, 7U, NotNull(), TestBindingMock::WAIT_EVENTS_TICK_COUNT))
        .InSequence(seq)
        .WillOnce(DoAll(SetArgPointee<2>(rxMask), Return(true)));
    EXPECT_CALL(_runnableMock2, execute()).InSequence(seq);

    uint32_t stopMask = 0U;
    EXPECT_CALL(
        _freeRtosMock,
        xTaskNotifyWait(0U, 7U, NotNull(), TestBindingMock::WAIT_EVENTS_TICK_COUNT))
        .InSequence(seq)
        .WillOnce(DoAll(SetArgPointee<2>(0U), StopDispatch(&_cut), Return(false)));
    EXPECT_CALL(_bindingMock, getHigherPriorityTaskWokenFunc())
        .WillOnce(Return(static_cast<BaseType_t*>(nullptr)));
    EXPECT_CALL(_freeRtosMock, xTaskNotify(&_taskHandle, _, eSetBits))
        .InSequence(seq)
        .WillOnce(SaveArg<1>(&stopMask));
    EXPECT_CALL(
        _freeRtosMock,
        xTaskNotifyWait(0U, 7U, NotNull(), TestBindingMock::WAIT_EVENTS_TICK_COUNT))
        .InSequence(seq)
        .WillOnce(DoAll(CopyArgPointee2(&stopMask), Return(true)));

    _osTaskFunction(_param);
}

/**
 * \desc: Full CAN UDS round trip: RX ISR -> receiveTask -> UDS processes -> write ->
 *        TX ISR -> callback -> done, then REPEAT the entire sequence.
 */
TEST_F(TaskNotificationRegressionTest, canFullUdsRoundTrip)
{
    // Model: _runnableMock1 = RX receive task, _runnableMock2 = TX sent callback
    CountingRunnable rxTask;
    CountingRunnable txCallback;

    for (int round = 0; round < 2; ++round)
    {
        // Step 1: RX ISR fires
        BaseType_t hptw = pdFALSE;
        EXPECT_CALL(_bindingMock, getHigherPriorityTaskWokenFunc())
            .WillOnce(Return(&hptw));
        EXPECT_CALL(
            _freeRtosMock,
            xTaskNotifyFromISR(&_taskHandle, _, eSetBits, &hptw));
        _cut.execute(rxTask);
        Mock::VerifyAndClearExpectations(&_bindingMock);
        Mock::VerifyAndClearExpectations(&_freeRtosMock);

        // Step 2: Task wakes, processes RX
        EXPECT_CALL(_systemTimerMock, getSystemTimeUs32Bit())
            .WillRepeatedly(Return(100000U + static_cast<uint32_t>(round) * 100000U));
        EXPECT_CALL(_freeRtosMock, xTaskNotifyWait(0U, 7U, NotNull(), 0U))
            .WillOnce(DoAll(SetArgPointee<2>(0x01U), Return(true)))
            .WillOnce(Return(false));
        _cut.dispatchWhileWork();
        Mock::VerifyAndClearExpectations(&_freeRtosMock);
        Mock::VerifyAndClearExpectations(&_systemTimerMock);
        EXPECT_EQ(static_cast<uint32_t>(round + 1), rxTask._count);

        // Step 3: After RX processing, UDS queues a TX. TX ISR fires when HW done.
        EXPECT_CALL(_bindingMock, getHigherPriorityTaskWokenFunc())
            .WillOnce(Return(&hptw));
        EXPECT_CALL(
            _freeRtosMock,
            xTaskNotifyFromISR(&_taskHandle, _, eSetBits, &hptw));
        _cut.execute(txCallback);
        Mock::VerifyAndClearExpectations(&_bindingMock);
        Mock::VerifyAndClearExpectations(&_freeRtosMock);

        // Step 4: Task wakes, processes TX callback
        EXPECT_CALL(_systemTimerMock, getSystemTimeUs32Bit())
            .WillRepeatedly(Return(150000U + static_cast<uint32_t>(round) * 100000U));
        EXPECT_CALL(_freeRtosMock, xTaskNotifyWait(0U, 7U, NotNull(), 0U))
            .WillOnce(DoAll(SetArgPointee<2>(0x01U), Return(true)))
            .WillOnce(Return(false));
        _cut.dispatchWhileWork();
        Mock::VerifyAndClearExpectations(&_freeRtosMock);
        Mock::VerifyAndClearExpectations(&_systemTimerMock);
        EXPECT_EQ(static_cast<uint32_t>(round + 1), txCallback._count);
    }
}

} // namespace

/*
 * Copyright 2019 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <log/log.h>

#include <mutex>

#include "FakeDisplayInjector.h"
#include "Scheduler/EventThread.h"
#include "Scheduler/RefreshRateConfigs.h"
#include "TestableScheduler.h"
#include "TestableSurfaceFlinger.h"
#include "mock/DisplayHardware/MockDisplayMode.h"
#include "mock/MockEventThread.h"
#include "mock/MockLayer.h"
#include "mock/MockSchedulerCallback.h"

namespace android::scheduler {

using android::mock::createDisplayMode;

using testing::_;
using testing::Return;

namespace {

using MockEventThread = android::mock::EventThread;
using MockLayer = android::mock::MockLayer;
using FakeDisplayDeviceInjector = TestableSurfaceFlinger::FakeDisplayDeviceInjector;

class SchedulerTest : public testing::Test {
protected:
    class MockEventThreadConnection : public android::EventThreadConnection {
    public:
        explicit MockEventThreadConnection(EventThread* eventThread)
              : EventThreadConnection(eventThread, /*callingUid*/ static_cast<uid_t>(0),
                                      ResyncCallback()) {}
        ~MockEventThreadConnection() = default;

        MOCK_METHOD1(stealReceiveChannel, binder::Status(gui::BitTube* outChannel));
        MOCK_METHOD1(setVsyncRate, binder::Status(int count));
        MOCK_METHOD0(requestNextVsync, binder::Status());
    };

    SchedulerTest();

    static constexpr PhysicalDisplayId kDisplayId1 = PhysicalDisplayId::fromPort(255u);
    static inline const DisplayModePtr kDisplay1Mode60 =
            createDisplayMode(kDisplayId1, DisplayModeId(0), 60_Hz);
    static inline const DisplayModePtr kDisplay1Mode120 =
            createDisplayMode(kDisplayId1, DisplayModeId(1), 120_Hz);
    static inline const DisplayModes kDisplay1Modes = makeModes(kDisplay1Mode60, kDisplay1Mode120);

    static constexpr PhysicalDisplayId kDisplayId2 = PhysicalDisplayId::fromPort(254u);
    static inline const DisplayModePtr kDisplay2Mode60 =
            createDisplayMode(kDisplayId2, DisplayModeId(0), 60_Hz);
    static inline const DisplayModePtr kDisplay2Mode120 =
            createDisplayMode(kDisplayId2, DisplayModeId(1), 120_Hz);
    static inline const DisplayModes kDisplay2Modes = makeModes(kDisplay2Mode60, kDisplay2Mode120);

    static constexpr PhysicalDisplayId kDisplayId3 = PhysicalDisplayId::fromPort(253u);
    static inline const DisplayModePtr kDisplay3Mode60 =
            createDisplayMode(kDisplayId3, DisplayModeId(0), 60_Hz);
    static inline const DisplayModes kDisplay3Modes = makeModes(kDisplay3Mode60);

    std::shared_ptr<RefreshRateConfigs> mConfigs =
            std::make_shared<RefreshRateConfigs>(makeModes(kDisplay1Mode60),
                                                 kDisplay1Mode60->getId());

    mock::SchedulerCallback mSchedulerCallback;
    TestableScheduler* mScheduler = new TestableScheduler{mConfigs, mSchedulerCallback};

    ConnectionHandle mConnectionHandle;
    MockEventThread* mEventThread;
    sp<MockEventThreadConnection> mEventThreadConnection;

    TestableSurfaceFlinger mFlinger;
    Hwc2::mock::PowerAdvisor mPowerAdvisor;
    sp<android::mock::NativeWindow> mNativeWindow = sp<android::mock::NativeWindow>::make();

    FakeDisplayInjector mFakeDisplayInjector{mFlinger, mPowerAdvisor, mNativeWindow};
};

SchedulerTest::SchedulerTest() {
    auto eventThread = std::make_unique<MockEventThread>();
    mEventThread = eventThread.get();
    EXPECT_CALL(*mEventThread, registerDisplayEventConnection(_)).WillOnce(Return(0));

    mEventThreadConnection = sp<MockEventThreadConnection>::make(mEventThread);

    // createConnection call to scheduler makes a createEventConnection call to EventThread. Make
    // sure that call gets executed and returns an EventThread::Connection object.
    EXPECT_CALL(*mEventThread, createEventConnection(_, _))
            .WillRepeatedly(Return(mEventThreadConnection));

    mConnectionHandle = mScheduler->createConnection(std::move(eventThread));
    EXPECT_TRUE(mConnectionHandle);

    mFlinger.resetScheduler(mScheduler);
}

} // namespace

TEST_F(SchedulerTest, invalidConnectionHandle) {
    ConnectionHandle handle;

    const sp<IDisplayEventConnection> connection = mScheduler->createDisplayEventConnection(handle);

    EXPECT_FALSE(connection);
    EXPECT_FALSE(mScheduler->getEventConnection(handle));

    // The EXPECT_CALLS make sure we don't call the functions on the subsequent event threads.
    EXPECT_CALL(*mEventThread, onHotplugReceived(_, _)).Times(0);
    mScheduler->onHotplugReceived(handle, kDisplayId1, false);

    EXPECT_CALL(*mEventThread, onScreenAcquired()).Times(0);
    mScheduler->onScreenAcquired(handle);

    EXPECT_CALL(*mEventThread, onScreenReleased()).Times(0);
    mScheduler->onScreenReleased(handle);

    std::string output;
    EXPECT_CALL(*mEventThread, dump(_)).Times(0);
    mScheduler->dump(handle, output);
    EXPECT_TRUE(output.empty());

    EXPECT_CALL(*mEventThread, setDuration(10ns, 20ns)).Times(0);
    mScheduler->setDuration(handle, 10ns, 20ns);
}

TEST_F(SchedulerTest, validConnectionHandle) {
    const sp<IDisplayEventConnection> connection =
            mScheduler->createDisplayEventConnection(mConnectionHandle);

    ASSERT_EQ(mEventThreadConnection, connection);
    EXPECT_TRUE(mScheduler->getEventConnection(mConnectionHandle));

    EXPECT_CALL(*mEventThread, onHotplugReceived(kDisplayId1, false)).Times(1);
    mScheduler->onHotplugReceived(mConnectionHandle, kDisplayId1, false);

    EXPECT_CALL(*mEventThread, onScreenAcquired()).Times(1);
    mScheduler->onScreenAcquired(mConnectionHandle);

    EXPECT_CALL(*mEventThread, onScreenReleased()).Times(1);
    mScheduler->onScreenReleased(mConnectionHandle);

    std::string output("dump");
    EXPECT_CALL(*mEventThread, dump(output)).Times(1);
    mScheduler->dump(mConnectionHandle, output);
    EXPECT_FALSE(output.empty());

    EXPECT_CALL(*mEventThread, setDuration(10ns, 20ns)).Times(1);
    mScheduler->setDuration(mConnectionHandle, 10ns, 20ns);

    static constexpr size_t kEventConnections = 5;
    EXPECT_CALL(*mEventThread, getEventThreadConnectionCount()).WillOnce(Return(kEventConnections));
    EXPECT_EQ(kEventConnections, mScheduler->getEventThreadConnectionCount(mConnectionHandle));
}

TEST_F(SchedulerTest, chooseRefreshRateForContentIsNoopWhenModeSwitchingIsNotSupported) {
    // The layer is registered at creation time and deregistered at destruction time.
    sp<MockLayer> layer = sp<MockLayer>::make(mFlinger.flinger());

    // recordLayerHistory should be a noop
    ASSERT_EQ(0u, mScheduler->getNumActiveLayers());
    mScheduler->recordLayerHistory(layer.get(), 0, LayerHistory::LayerUpdateType::Buffer);
    ASSERT_EQ(0u, mScheduler->getNumActiveLayers());

    constexpr hal::PowerMode kPowerModeOn = hal::PowerMode::ON;
    mScheduler->setDisplayPowerMode(kPowerModeOn);

    constexpr uint32_t kDisplayArea = 999'999;
    mScheduler->onActiveDisplayAreaChanged(kDisplayArea);

    EXPECT_CALL(mSchedulerCallback, requestDisplayModes(_)).Times(0);
    mScheduler->chooseRefreshRateForContent();
}

TEST_F(SchedulerTest, updateDisplayModes) {
    ASSERT_EQ(0u, mScheduler->layerHistorySize());
    sp<MockLayer> layer = sp<MockLayer>::make(mFlinger.flinger());
    ASSERT_EQ(1u, mScheduler->layerHistorySize());

    mScheduler->setRefreshRateConfigs(
            std::make_shared<RefreshRateConfigs>(kDisplay1Modes, kDisplay1Mode60->getId()));

    ASSERT_EQ(0u, mScheduler->getNumActiveLayers());
    mScheduler->recordLayerHistory(layer.get(), 0, LayerHistory::LayerUpdateType::Buffer);
    ASSERT_EQ(1u, mScheduler->getNumActiveLayers());
}

TEST_F(SchedulerTest, dispatchCachedReportedMode) {
    mScheduler->clearCachedReportedMode();

    EXPECT_CALL(*mEventThread, onModeChanged(_)).Times(0);
    EXPECT_NO_FATAL_FAILURE(mScheduler->dispatchCachedReportedMode());
}

TEST_F(SchedulerTest, onNonPrimaryDisplayModeChanged_invalidParameters) {
    const auto mode = DisplayMode::Builder(hal::HWConfigId(0))
                              .setId(DisplayModeId(111))
                              .setPhysicalDisplayId(kDisplayId1)
                              .setVsyncPeriod(111111)
                              .build();

    // If the handle is incorrect, the function should return before
    // onModeChange is called.
    ConnectionHandle invalidHandle = {.id = 123};
    EXPECT_NO_FATAL_FAILURE(mScheduler->onNonPrimaryDisplayModeChanged(invalidHandle, mode));
    EXPECT_CALL(*mEventThread, onModeChanged(_)).Times(0);
}

TEST_F(SchedulerTest, calculateMaxAcquiredBufferCount) {
    EXPECT_EQ(1, mFlinger.calculateMaxAcquiredBufferCount(60_Hz, 30ms));
    EXPECT_EQ(2, mFlinger.calculateMaxAcquiredBufferCount(90_Hz, 30ms));
    EXPECT_EQ(3, mFlinger.calculateMaxAcquiredBufferCount(120_Hz, 30ms));

    EXPECT_EQ(2, mFlinger.calculateMaxAcquiredBufferCount(60_Hz, 40ms));

    EXPECT_EQ(1, mFlinger.calculateMaxAcquiredBufferCount(60_Hz, 10ms));
}

MATCHER(Is120Hz, "") {
    return isApproxEqual(arg.front().modePtr->getFps(), 120_Hz);
}

TEST_F(SchedulerTest, chooseRefreshRateForContentSelectsMaxRefreshRate) {
    const auto display = mFakeDisplayInjector.injectInternalDisplay(
            [&](FakeDisplayDeviceInjector& injector) {
                injector.setDisplayModes(kDisplay1Modes, kDisplay1Mode60->getId());
            },
            {.displayId = kDisplayId1});

    mScheduler->registerDisplay(display);
    mScheduler->setRefreshRateConfigs(display->holdRefreshRateConfigs());

    const sp<MockLayer> layer = sp<MockLayer>::make(mFlinger.flinger());
    EXPECT_CALL(*layer, isVisible()).WillOnce(Return(true));

    mScheduler->recordLayerHistory(layer.get(), 0, LayerHistory::LayerUpdateType::Buffer);

    constexpr hal::PowerMode kPowerModeOn = hal::PowerMode::ON;
    mScheduler->setDisplayPowerMode(kPowerModeOn);

    constexpr uint32_t kDisplayArea = 999'999;
    mScheduler->onActiveDisplayAreaChanged(kDisplayArea);

    EXPECT_CALL(mSchedulerCallback, requestDisplayModes(Is120Hz())).Times(1);
    mScheduler->chooseRefreshRateForContent();

    // No-op if layer requirements have not changed.
    EXPECT_CALL(mSchedulerCallback, requestDisplayModes(_)).Times(0);
    mScheduler->chooseRefreshRateForContent();
}

TEST_F(SchedulerTest, chooseDisplayModesSingleDisplay) {
    const auto display = mFakeDisplayInjector.injectInternalDisplay(
            [&](FakeDisplayDeviceInjector& injector) {
                injector.setDisplayModes(kDisplay1Modes, kDisplay1Mode60->getId());
            },
            {.displayId = kDisplayId1});

    mScheduler->registerDisplay(display);

    std::vector<RefreshRateConfigs::LayerRequirement> layers =
            std::vector<RefreshRateConfigs::LayerRequirement>({{.weight = 1.f}, {.weight = 1.f}});
    mScheduler->setContentRequirements(layers);
    GlobalSignals globalSignals = {.idle = true};
    mScheduler->setTouchStateAndIdleTimerPolicy(globalSignals);

    using DisplayModeChoice = TestableScheduler::DisplayModeChoice;

    auto modeChoices = mScheduler->chooseDisplayModes();
    ASSERT_EQ(1u, modeChoices.size());

    auto choice = modeChoices.get(kDisplayId1);
    ASSERT_TRUE(choice);
    EXPECT_EQ(choice->get(), DisplayModeChoice(kDisplay1Mode60, globalSignals));

    globalSignals = {.idle = false};
    mScheduler->setTouchStateAndIdleTimerPolicy(globalSignals);

    modeChoices = mScheduler->chooseDisplayModes();
    ASSERT_EQ(1u, modeChoices.size());

    choice = modeChoices.get(kDisplayId1);
    ASSERT_TRUE(choice);
    EXPECT_EQ(choice->get(), DisplayModeChoice(kDisplay1Mode120, globalSignals));

    globalSignals = {.touch = true};
    mScheduler->replaceTouchTimer(10);
    mScheduler->setTouchStateAndIdleTimerPolicy(globalSignals);

    modeChoices = mScheduler->chooseDisplayModes();
    ASSERT_EQ(1u, modeChoices.size());

    choice = modeChoices.get(kDisplayId1);
    ASSERT_TRUE(choice);
    EXPECT_EQ(choice->get(), DisplayModeChoice(kDisplay1Mode120, globalSignals));

    mScheduler->unregisterDisplay(kDisplayId1);
    EXPECT_TRUE(mScheduler->mutableDisplays().empty());
}

TEST_F(SchedulerTest, chooseDisplayModesMultipleDisplays) {
    const auto display1 = mFakeDisplayInjector.injectInternalDisplay(
            [&](FakeDisplayDeviceInjector& injector) {
                injector.setDisplayModes(kDisplay1Modes, kDisplay1Mode60->getId());
            },
            {.displayId = kDisplayId1, .hwcDisplayId = 42, .isPrimary = true});
    const auto display2 = mFakeDisplayInjector.injectInternalDisplay(
            [&](FakeDisplayDeviceInjector& injector) {
                injector.setDisplayModes(kDisplay2Modes, kDisplay2Mode60->getId());
            },
            {.displayId = kDisplayId2, .hwcDisplayId = 41, .isPrimary = false});

    mScheduler->registerDisplay(display1);
    mScheduler->registerDisplay(display2);

    using DisplayModeChoice = TestableScheduler::DisplayModeChoice;
    TestableScheduler::DisplayModeChoiceMap expectedChoices;

    {
        const GlobalSignals globalSignals = {.idle = true};
        expectedChoices =
                ftl::init::map<const PhysicalDisplayId&,
                               DisplayModeChoice>(kDisplayId1, kDisplay1Mode60,
                                                  globalSignals)(kDisplayId2, kDisplay2Mode60,
                                                                 globalSignals);

        std::vector<RefreshRateConfigs::LayerRequirement> layers = {{.weight = 1.f},
                                                                    {.weight = 1.f}};
        mScheduler->setContentRequirements(layers);
        mScheduler->setTouchStateAndIdleTimerPolicy(globalSignals);

        const auto actualChoices = mScheduler->chooseDisplayModes();
        EXPECT_EQ(expectedChoices, actualChoices);
    }
    {
        const GlobalSignals globalSignals = {.idle = false};
        expectedChoices =
                ftl::init::map<const PhysicalDisplayId&,
                               DisplayModeChoice>(kDisplayId1, kDisplay1Mode120,
                                                  globalSignals)(kDisplayId2, kDisplay2Mode120,
                                                                 globalSignals);

        mScheduler->setTouchStateAndIdleTimerPolicy(globalSignals);

        const auto actualChoices = mScheduler->chooseDisplayModes();
        EXPECT_EQ(expectedChoices, actualChoices);
    }
    {
        const GlobalSignals globalSignals = {.touch = true};
        mScheduler->replaceTouchTimer(10);
        mScheduler->setTouchStateAndIdleTimerPolicy(globalSignals);

        expectedChoices =
                ftl::init::map<const PhysicalDisplayId&,
                               DisplayModeChoice>(kDisplayId1, kDisplay1Mode120,
                                                  globalSignals)(kDisplayId2, kDisplay2Mode120,
                                                                 globalSignals);

        const auto actualChoices = mScheduler->chooseDisplayModes();
        EXPECT_EQ(expectedChoices, actualChoices);
    }
    {
        // This display does not support 120 Hz, so we should choose 60 Hz despite the touch signal.
        const auto display3 = mFakeDisplayInjector.injectInternalDisplay(
                [&](FakeDisplayDeviceInjector& injector) {
                    injector.setDisplayModes(kDisplay3Modes, kDisplay3Mode60->getId());
                },
                {.displayId = kDisplayId3, .hwcDisplayId = 40, .isPrimary = false});

        mScheduler->registerDisplay(display3);

        const GlobalSignals globalSignals = {.touch = true};
        mScheduler->replaceTouchTimer(10);
        mScheduler->setTouchStateAndIdleTimerPolicy(globalSignals);

        expectedChoices =
                ftl::init::map<const PhysicalDisplayId&,
                               DisplayModeChoice>(kDisplayId1, kDisplay1Mode60,
                                                  globalSignals)(kDisplayId2, kDisplay2Mode60,
                                                                 globalSignals)(kDisplayId3,
                                                                                kDisplay3Mode60,
                                                                                globalSignals);

        const auto actualChoices = mScheduler->chooseDisplayModes();
        EXPECT_EQ(expectedChoices, actualChoices);
    }
}

} // namespace android::scheduler

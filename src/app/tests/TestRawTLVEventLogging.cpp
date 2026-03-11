/*
 *
 *    Copyright (c) 2021 Project CHIP Authors
 *    All rights reserved.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *        http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

#include <access/SubjectDescriptor.h>
#include <app/EventLogging.h>
#include <app/EventManagement.h>
#include <app/InteractionModelEngine.h>
#include <app/MessageDef/EventDataIB.h>
#include <app/MessageDef/EventReportIB.h>
#include <app/tests/AppTestContext.h>
#include <data-model-providers/codegen/Instance.h>
#include <lib/core/CHIPCore.h>
#include <lib/core/TLV.h>
#include <lib/core/TLVDebug.h>
#include <lib/core/TLVUtilities.h>
#include <lib/support/CHIPCounter.h>
#include <lib/support/CodeUtils.h>

#include <lib/core/StringBuilderAdapters.h>
#include <pw_unit_test/framework.h>

namespace {

using namespace chip;
using namespace chip::app;
using namespace chip::TLV;

static constexpr ClusterId kTestClusterId    = 0x00000022;
static constexpr uint32_t kTestEventId       = 1;
static constexpr EndpointId kTestEndpointId  = 2;
static constexpr Tag kTestFieldTag           = ContextTag(1);
static constexpr int32_t kTestFieldValue     = 42;

static uint8_t gDebugEventBuffer[256];
static uint8_t gInfoEventBuffer[256];
static uint8_t gCritEventBuffer[256];
static CircularEventBuffer gCircularEventBuffer[3];

class TestRawTLVEventLogging : public chip::Testing::AppContext
{
public:
    void SetUp() override
    {
        const LogStorageResources logStorageResources[] = {
            { &gDebugEventBuffer[0], sizeof(gDebugEventBuffer), PriorityLevel::Debug },
            { &gInfoEventBuffer[0], sizeof(gInfoEventBuffer), PriorityLevel::Info },
            { &gCritEventBuffer[0], sizeof(gCritEventBuffer), PriorityLevel::Critical },
        };

        AppContext::SetUp();
        InteractionModelEngine::GetInstance()->SetDataModelProvider(CodegenDataModelProviderInstance(nullptr));
        ASSERT_EQ(mEventCounter.Init(0), CHIP_NO_ERROR);
        EventManagement::CreateEventManagement(&GetExchangeManager(), MATTER_ARRAY_SIZE(logStorageResources),
                                               gCircularEventBuffer, logStorageResources, &mEventCounter);
    }

    void TearDown() override
    {
        EventManagement::DestroyEventManagement();
        AppContext::TearDown();
    }

private:
    MonotonicallyIncreasingCounter<EventNumber> mEventCounter;
};

/**
 * Helper: encode a simple TLV structure { ContextTag(1): int32 value } into the
 * provided buffer and return the number of bytes written.
 */
static CHIP_ERROR EncodeTestEventTLV(uint8_t * buf, size_t bufLen, size_t & encodedLen, int32_t fieldValue)
{
    TLVWriter writer;
    writer.Init(buf, bufLen);

    TLVType containerType;
    ReturnErrorOnFailure(writer.StartContainer(AnonymousTag(), kTLVType_Structure, containerType));
    ReturnErrorOnFailure(writer.Put(kTestFieldTag, fieldValue));
    ReturnErrorOnFailure(writer.EndContainer(containerType));
    ReturnErrorOnFailure(writer.Finalize());

    encodedLen = writer.GetLengthWritten();
    return CHIP_NO_ERROR;
}

static size_t GetEventCount(EventManagement & logMgmt, PriorityLevel priority)
{
    TLVReader reader;
    size_t count = 0;
    CircularEventBufferWrapper bufWrapper;
    VerifyOrDie(logMgmt.GetEventReader(reader, priority, &bufWrapper) == CHIP_NO_ERROR);
    VerifyOrDie(Utilities::Count(reader, count, false) == CHIP_NO_ERROR);
    return count;
}

TEST_F(TestRawTLVEventLogging, TestLogSingleRawTLVEvent)
{
    uint8_t tlvBuf[64];
    size_t tlvLen = 0;
    ASSERT_EQ(EncodeTestEventTLV(tlvBuf, sizeof(tlvBuf), tlvLen, kTestFieldValue), CHIP_NO_ERROR);

    EventNumber eventNumber;
    CHIP_ERROR err = LogEvent(ConcreteEventPath(kTestEndpointId, kTestClusterId, kTestEventId), PriorityLevel::Info, tlvBuf, tlvLen,
                              eventNumber);
    EXPECT_EQ(err, CHIP_NO_ERROR);

    EXPECT_EQ(GetEventCount(EventManagement::GetInstance(), PriorityLevel::Debug), 1u);
}

TEST_F(TestRawTLVEventLogging, TestLogMultipleRawTLVEvents)
{
    EventManagement & logMgmt = EventManagement::GetInstance();
    EventNumber eid1 = 0, eid2 = 0, eid3 = 0;

    uint8_t tlvBuf[64];
    size_t tlvLen = 0;

    ASSERT_EQ(EncodeTestEventTLV(tlvBuf, sizeof(tlvBuf), tlvLen, 10), CHIP_NO_ERROR);
    EXPECT_EQ(LogEvent(ConcreteEventPath(kTestEndpointId, kTestClusterId, kTestEventId), PriorityLevel::Info, tlvBuf, tlvLen, eid1),
              CHIP_NO_ERROR);

    ASSERT_EQ(EncodeTestEventTLV(tlvBuf, sizeof(tlvBuf), tlvLen, 20), CHIP_NO_ERROR);
    EXPECT_EQ(LogEvent(ConcreteEventPath(kTestEndpointId, kTestClusterId, kTestEventId), PriorityLevel::Info, tlvBuf, tlvLen, eid2),
              CHIP_NO_ERROR);

    ASSERT_EQ(EncodeTestEventTLV(tlvBuf, sizeof(tlvBuf), tlvLen, 30), CHIP_NO_ERROR);
    EXPECT_EQ(LogEvent(ConcreteEventPath(kTestEndpointId, kTestClusterId, kTestEventId), PriorityLevel::Info, tlvBuf, tlvLen, eid3),
              CHIP_NO_ERROR);

    EXPECT_EQ((eid1 + 1), eid2);
    EXPECT_EQ((eid2 + 1), eid3);
    EXPECT_EQ(GetEventCount(logMgmt, PriorityLevel::Debug), 3u);
}

TEST_F(TestRawTLVEventLogging, TestLogRawTLVEventAtDifferentPriorities)
{
    EventManagement & logMgmt = EventManagement::GetInstance();

    uint8_t tlvBuf[64];
    size_t tlvLen    = 0;
    EventNumber eid  = 0;

    ASSERT_EQ(EncodeTestEventTLV(tlvBuf, sizeof(tlvBuf), tlvLen, 1), CHIP_NO_ERROR);
    EXPECT_EQ(
        LogEvent(ConcreteEventPath(kTestEndpointId, kTestClusterId, kTestEventId), PriorityLevel::Debug, tlvBuf, tlvLen, eid),
        CHIP_NO_ERROR);

    ASSERT_EQ(EncodeTestEventTLV(tlvBuf, sizeof(tlvBuf), tlvLen, 2), CHIP_NO_ERROR);
    EXPECT_EQ(
        LogEvent(ConcreteEventPath(kTestEndpointId, kTestClusterId, kTestEventId), PriorityLevel::Info, tlvBuf, tlvLen, eid),
        CHIP_NO_ERROR);

    ASSERT_EQ(EncodeTestEventTLV(tlvBuf, sizeof(tlvBuf), tlvLen, 3), CHIP_NO_ERROR);
    EXPECT_EQ(
        LogEvent(ConcreteEventPath(kTestEndpointId, kTestClusterId, kTestEventId), PriorityLevel::Critical, tlvBuf, tlvLen, eid),
        CHIP_NO_ERROR);

    // All events are initially written to the lowest-priority (Debug) circular buffer.
    // They only get promoted to higher-priority buffers when the lower one overflows.
    // Since our buffers are large enough, all 3 events remain in the Debug buffer.
    // The reader chains backwards from the requested priority buffer through all
    // lower-priority buffers, so every priority level sees all 3 events.
    EXPECT_EQ(GetEventCount(logMgmt, PriorityLevel::Debug), 3u);
    EXPECT_EQ(GetEventCount(logMgmt, PriorityLevel::Info), 3u);
    EXPECT_EQ(GetEventCount(logMgmt, PriorityLevel::Critical), 3u);
}

TEST_F(TestRawTLVEventLogging, TestLogRawTLVEventWithInvalidTLV)
{
    uint8_t badTlv[] = { 0xFF, 0xFF, 0xFF };
    EventNumber eid  = 0;

    CHIP_ERROR err = LogEvent(ConcreteEventPath(kTestEndpointId, kTestClusterId, kTestEventId), PriorityLevel::Info, badTlv,
                              sizeof(badTlv), eid);
    EXPECT_NE(err, CHIP_NO_ERROR);
}

TEST_F(TestRawTLVEventLogging, TestLogRawTLVEventWithZeroLengthTLV)
{
    EventNumber eid = 0;

    CHIP_ERROR err =
        LogEvent(ConcreteEventPath(kTestEndpointId, kTestClusterId, kTestEventId), PriorityLevel::Info, nullptr, 0, eid);
    EXPECT_NE(err, CHIP_NO_ERROR);
}

TEST_F(TestRawTLVEventLogging, TestFetchLoggedRawTLVEventData)
{
    uint8_t tlvBuf[64];
    size_t tlvLen    = 0;
    EventNumber eid  = 0;

    ASSERT_EQ(EncodeTestEventTLV(tlvBuf, sizeof(tlvBuf), tlvLen, kTestFieldValue), CHIP_NO_ERROR);
    ASSERT_EQ(LogEvent(ConcreteEventPath(kTestEndpointId, kTestClusterId, kTestEventId), PriorityLevel::Info, tlvBuf, tlvLen, eid),
              CHIP_NO_ERROR);

    chip::Platform::ScopedMemoryBuffer<uint8_t> readBuf;
    VerifyOrDie(readBuf.Alloc(1024));

    TLVWriter writer;
    writer.Init(readBuf.Get(), 1024);

    SingleLinkedListNode<EventPathParams> path;
    path.mValue.mEndpointId = kTestEndpointId;
    path.mValue.mClusterId  = kTestClusterId;
    path.mValue.mEventId    = kTestEventId;

    size_t eventCount          = 0;
    EventNumber startingNumber = 0;
    CHIP_ERROR err = EventManagement::GetInstance().FetchEventsSince(writer, &path, startingNumber, eventCount,
                                                                    Access::SubjectDescriptor{});
    EXPECT_TRUE(err == CHIP_NO_ERROR || err == CHIP_END_OF_TLV);
    EXPECT_EQ(eventCount, 1u);

    TLVReader reader;
    reader.Init(readBuf.Get(), writer.GetLengthWritten());

    // Navigate into the EventReportIB array to find our data
    // Structure: EventReportIB -> EventDataIB -> Data { field }
    EXPECT_EQ(reader.Next(kTLVType_Structure, AnonymousTag()), CHIP_NO_ERROR);

    TLVType reportContainer;
    EXPECT_EQ(reader.EnterContainer(reportContainer), CHIP_NO_ERROR);

    // Find the EventDataIB element
    EXPECT_EQ(reader.Next(kTLVType_Structure, ContextTag(to_underlying(EventReportIB::Tag::kEventData))), CHIP_NO_ERROR);

    TLVType dataIBContainer;
    EXPECT_EQ(reader.EnterContainer(dataIBContainer), CHIP_NO_ERROR);

    // Walk through the EventDataIB fields to find kData
    bool foundData = false;
    while (reader.Next() == CHIP_NO_ERROR)
    {
        if (reader.GetTag() == ContextTag(to_underlying(EventDataIB::Tag::kData)))
        {
            TLVType dataContainer;
            EXPECT_EQ(reader.EnterContainer(dataContainer), CHIP_NO_ERROR);
            EXPECT_EQ(reader.Next(), CHIP_NO_ERROR);
            EXPECT_EQ(reader.GetTag(), kTestFieldTag);

            int32_t readValue = 0;
            EXPECT_EQ(reader.Get(readValue), CHIP_NO_ERROR);
            EXPECT_EQ(readValue, kTestFieldValue);
            foundData = true;

            EXPECT_EQ(reader.ExitContainer(dataContainer), CHIP_NO_ERROR);
            break;
        }
    }
    EXPECT_TRUE(foundData);
}

TEST_F(TestRawTLVEventLogging, TestRawTLVAndTypedEventCoexist)
{
    EventManagement & logMgmt = EventManagement::GetInstance();

    // Log a raw TLV event
    uint8_t tlvBuf[64];
    size_t tlvLen       = 0;
    EventNumber rawEid  = 0;

    ASSERT_EQ(EncodeTestEventTLV(tlvBuf, sizeof(tlvBuf), tlvLen, 100), CHIP_NO_ERROR);
    EXPECT_EQ(
        LogEvent(ConcreteEventPath(kTestEndpointId, kTestClusterId, kTestEventId), PriorityLevel::Info, tlvBuf, tlvLen, rawEid),
        CHIP_NO_ERROR);

    // Log another raw TLV event using the delegate directly
    EventNumber directEid = 0;
    ASSERT_EQ(EncodeTestEventTLV(tlvBuf, sizeof(tlvBuf), tlvLen, 200), CHIP_NO_ERROR);

    RawTLVEventLogger delegate(tlvBuf, tlvLen);
    EventOptions options;
    options.mPath     = ConcreteEventPath(kTestEndpointId, kTestClusterId, kTestEventId);
    options.mPriority = PriorityLevel::Info;
    EXPECT_EQ(logMgmt.LogEvent(&delegate, options, directEid), CHIP_NO_ERROR);

    EXPECT_EQ((rawEid + 1), directEid);
    EXPECT_EQ(GetEventCount(logMgmt, PriorityLevel::Debug), 2u);
}

} // namespace

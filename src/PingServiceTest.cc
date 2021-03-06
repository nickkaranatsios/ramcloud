/* Copyright (c) 2011-2013 Stanford University
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR(S) DISCLAIM ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL AUTHORS BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include<fstream>
#include "TestUtil.h"
#include "BindTransport.h"
#include "Common.h"
#include "PingClient.h"
#include "PingService.h"
#include "RawMetrics.h"
#include "ServerMetrics.h"
#include "TransportManager.h"
#include "ServerList.h"
#include "CoordinatorClient.h"
#include "Key.h"
#include "Tablets.pb.h"
#include "Tub.h"
#include "RamCloud.h"

// Note: this file tests both PingService.cc and PingClient.cc.

namespace RAMCloud {

// This class is designed to substitute class TebletMapFetcher
// in ObjectFinder so that the client can locate the proper server
// that owns a table without the need of going through the
// coordinator. We used this class, since we don't have cluster in
// this test.
class MockTabletMapFetcher : public ObjectFinder::TabletMapFetcher {
  public:

    /**
    * Constructor.
    *
    * \param tableId
    *      tableId that the server holds
    * \param locator 
    *      stringLocator of the server that holds the table  
    */
    explicit MockTabletMapFetcher(string locator, uint64_t tableId)
        : locator(locator)
        , tableId(tableId)
    {}
    void getTabletMap(ProtoBuf::Tablets& tabletMap) {
        tabletMap.clear_tablet();
        ProtoBuf::Tablets_Tablet& entry(*tabletMap.add_tablet());
        entry.set_table_id(tableId);
        entry.set_start_key_hash(0);
        entry.set_end_key_hash(~0UL);
        entry.set_state(ProtoBuf::Tablets_Tablet_State_NORMAL);
        entry.set_service_locator(locator);
    }
    string locator;
    uint64_t tableId;

};


class PingServiceTest : public ::testing::Test {
  public:
    Context context;
    ServerList serverList;
    BindTransport transport;
    TransportManager::MockRegistrar mockRegistrar;
    PingService pingService;
    ServerId serverId;
    Tub<RamCloud> ramcloud;

    PingServiceTest()
        : context()
        , serverList(&context)
        , transport(&context)
        , mockRegistrar(&context, transport)
        , pingService(&context)
        , serverId(1, 3)
        , ramcloud()
    {
        transport.addService(pingService, "mock:host=ping",
                             WireFormat::PING_SERVICE);
        serverList.testingAdd({serverId, "mock:host=ping",
                               {WireFormat::PING_SERVICE}, 100,
                               ServerStatus::UP});
        ramcloud.construct(&context, "mock:host=coordinator");
    }

    DISALLOW_COPY_AND_ASSIGN(PingServiceTest);
};

TEST_F(PingServiceTest, serverControl_DipatchProfilerBasics){
    TestLog::Enable _;
    uint64_t tableId = 1;
    string locator = serverList.getLocator(serverId);

    // We register an object of MockTabletMapFetcher that
    // is suposed to replace the tabletMapFetcher for the
    // Client.
    ramcloud->objectFinder.tabletMapFetcher.reset(
                            new MockTabletMapFetcher(locator, tableId));
    string str = ramcloud->objectFinder.lookupTablet(tableId, 0)
                                                .service_locator();
    ASSERT_STREQ(str.c_str(), locator.c_str());
    Buffer output;
    uint64_t totalElements = 50000000;

    // Testing basics of operation START_DISPATCH_PROFILER.
    ASSERT_FALSE(pingService.context->dispatch->profilerFlag);
    ramcloud->serverControl(tableId, "0", 1,
                            WireFormat::START_DISPATCH_PROFILER,
                            &totalElements, sizeof32(totalElements),
                            &output);
    ASSERT_TRUE(pingService.context->dispatch->profilerFlag);
    ASSERT_EQ(totalElements,
                pingService.context->dispatch->totalElements);

    // Testing basics of operation STOP_DISPATCH_PROFILER.
    ramcloud->serverControl(tableId, "0", 1,
                            WireFormat::STOP_DISPATCH_PROFILER,
                            " ", 1, &output);
    ASSERT_FALSE(pingService.context->dispatch->profilerFlag);

    // Testing basics of operation STOP_DISPATCH_PROFILER.
    ramcloud->serverControl(tableId, "0", 1,
                            WireFormat::DUMP_DISPATCH_PROFILE,
                            "pollingTimesTestFile.txt", 25, &output);
    std::ifstream stream("pollingTimesTestFile.txt");
    EXPECT_FALSE(stream.fail());
    stream.close();
    remove("pollingTimesTestFile.txt");

    // Testing unimplemented ControlOp.
    EXPECT_THROW(ramcloud->serverControl(tableId, "0", 1,
                                        WireFormat::ControlOp(0),
                                        "File.txt", 9, &output)
                 , UnimplementedRequestError);
}

TEST_F(PingServiceTest, serverControl_DispatchProfilerExceptions) {
    TestLog::Enable _;
    uint64_t tableId = 2;
    string locator = serverList.getLocator(serverId);
    ramcloud->objectFinder.tabletMapFetcher.reset(
                            new MockTabletMapFetcher(locator, tableId));
    Buffer output;
    uint32_t totalElements = 10000000;

    // Testing MessageTooShortError for ControlOp
    // WireFormat::START_DISPATCH_PROFILER
    EXPECT_THROW(ramcloud->serverControl(tableId, "0", 1,
                            WireFormat::START_DISPATCH_PROFILER,
                            &totalElements, sizeof32(totalElements),
                            &output)
                , MessageTooShortError);

    // Testing RequestFormatError for ControlOp
    // WireFormat::DUMP_DISPATCH_PROFILER
    EXPECT_THROW(ramcloud->serverControl(tableId, "0", 1,
                            WireFormat::DUMP_DISPATCH_PROFILE,
                            "pollingTimesTestFile.txt", 24, &output)
                , RequestFormatError);
    EXPECT_THROW(ramcloud->serverControl(tableId, "0", 1,
                            WireFormat::DUMP_DISPATCH_PROFILE,
                            "FolderNotExisting/File.txt", 27, &output)
                , RequestFormatError);
}

TEST_F(PingServiceTest, ping_basics) {
    TestLog::Enable _;
    PingClient::ping(&context, serverId);
    EXPECT_EQ("", TestLog::get());
    TestLog::reset();
    PingClient::ping(&context, serverId, serverId);
    EXPECT_EQ("ping: Received ping request from server 1.3", TestLog::get());
    TestLog::reset();
    EXPECT_THROW(PingClient::ping(&context, serverId, ServerId(99)),
                CallerNotInClusterException);
    EXPECT_EQ("ping: Received ping request from server 99.0",
              TestLog::get());
}

TEST_F(PingServiceTest, ping_wait_timeout) {
    TestLog::Enable _;
    ServerId serverId2(2, 3);
    MockTransport mockTransport(&context);
    context.transportManager->registerMock(&mockTransport, "mock2");
    serverList.testingAdd({serverId2, "mock2:", {WireFormat::PING_SERVICE}, 100,
                           ServerStatus::UP});
    PingRpc rpc(&context, serverId2);
    uint64_t start = Cycles::rdtsc();
    EXPECT_FALSE(rpc.wait(1000000));
    EXPECT_EQ("wait: timeout", TestLog::get());
    double elapsedMicros = 1e06* Cycles::toSeconds(Cycles::rdtsc() - start);
    EXPECT_GE(elapsedMicros, 1000.0);
    EXPECT_LE(elapsedMicros, 2000.0);
}

// Helper function that runs in a separate thread for the following test.
static void pingThread(PingRpc* rpc, bool* result) {
    *result = rpc->wait(100000000);
}

TEST_F(PingServiceTest, ping_wait_serverGoesAway) {
    TestLog::Enable _;
    ServerId serverId2(2, 3);
    MockTransport mockTransport(&context);
    context.transportManager->registerMock(&mockTransport, "mock2");
    serverList.testingAdd({serverId2, "mock2:", {WireFormat::PING_SERVICE}, 100,
                           ServerStatus::UP});
    bool result = 0;
    PingRpc rpc(&context, serverId2);
    std::thread thread(pingThread, &rpc, &result);
    usleep(100);
    EXPECT_EQ(0LU, result);

    // Delete the server, then fail the ping RPC so that there is a retry
    // that discovers that targetId is gone.
    serverList.testingRemove(serverId2);
    mockTransport.lastNotifier->failed();

    // Give the other thread a chance to finish.
    for (int i = 0; (result == 0) && (i < 1000); i++) {
        usleep(100);
    }

    EXPECT_FALSE(result);
    EXPECT_EQ("wait: server doesn't exist", TestLog::get());
    thread.join();
}

TEST_F(PingServiceTest, ping_wait_exception) {
    TestLog::Enable _;
    PingRpc rpc(&context, serverId, ServerId(99));
    EXPECT_THROW(rpc.wait(100000), CallerNotInClusterException);
}

TEST_F(PingServiceTest, ping_wait_success) {
    TestLog::Enable _;
    PingRpc rpc(&context, serverId);
    EXPECT_TRUE(rpc.wait(100000));
}

TEST_F(PingServiceTest, proxyPing_basics) {
    uint64_t ns = PingClient::proxyPing(&context, serverId, serverId, 100000);
    EXPECT_NE(-1U, ns);
    EXPECT_LT(10U, ns);
}
TEST_F(PingServiceTest, proxyPing_timeout) {
    // Test the situation where the target times out.
    ServerId targetId(2, 3);
    MockTransport mockTransport(&context);
    context.transportManager->registerMock(&mockTransport, "mock2");
    serverList.testingAdd({targetId, "mock2:", {WireFormat::PING_SERVICE}, 100,
                           ServerStatus::UP});
    uint64_t start = Cycles::rdtsc();
    EXPECT_EQ(0xffffffffffffffffU,
              PingClient::proxyPing(&context, serverId, targetId, 1000000));
    double elapsedMicros = 1e06* Cycles::toSeconds(Cycles::rdtsc() - start);
    EXPECT_GE(elapsedMicros, 1000.0);
    EXPECT_LE(elapsedMicros, 2000.0);
}

} // namespace RAMCloud

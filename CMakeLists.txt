cmake_minimum_required(VERSION 4.3.5)

project(bus)
enable_testing()

find_package(Protobuf REQUIRED)
include_directories(${Protobuf_INCLUDE_DIRS})
include_directories(${CMAKE_CURRENT_BINARY_DIR})

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}")

protobuf_generate_cpp(LIB_PROTO_SRCS LIB_PROTO_HDRS service.proto)
protobuf_generate_cpp(PROTO_SRCS PROTO_HDRS messages.proto)

add_library(bus
    bus.h bus.cpp
    proto_bus.h proto_bus.cpp
    connect_pool.h connect_pool.cpp
    endpoint_manager.h endpoint_manager.cpp
    error.h error.cpp
    util.h util.cpp
    ${LIB_PROTO_HDRS} ${LIB_PROTO_SRCS})

# old cmake
find_package(Threads::Threads QUIET)
target_link_libraries(bus PUBLIC Threads::Threads)

add_executable(testBus testBus.cpp
    ${PROTO_SRCS}
    ${PROTO_HDRS})

target_link_libraries(testBus ${Protobuf_LIBRARIES} bus)

add_executable(testService testService.cpp
    ${PROTO_SRCS}
    ${PROTO_HDRS})

target_link_libraries(testService ${Protobuf_LIBRARIES} bus)

add_executable(benchProxy benchProxy.cpp
    ${PROTO_SRCS}
    ${PROTO_HDRS})

target_link_libraries(benchProxy ${Protobuf_LIBRARIES} bus)

add_test(NAME bus COMMAND testBus)
add_test(NAME service COMMAND testService)

# Regression tests for known bugs: one CTest entry per bug (expected to fail
# until the corresponding bug is fixed).
find_package(GTest REQUIRED)

add_executable(testBugs testBugs.cpp test_helpers.h
    ${PROTO_SRCS}
    ${PROTO_HDRS})
target_link_libraries(testBugs ${Protobuf_LIBRARIES} bus GTest::gtest GTest::gtest_main)

add_executable(testBugsInterpose testBugsInterpose.cpp test_helpers.h
    ${PROTO_SRCS}
    ${PROTO_HDRS})
target_link_libraries(testBugsInterpose ${Protobuf_LIBRARIES} bus GTest::gtest GTest::gtest_main ${CMAKE_DL_LIBS})

function(add_bug_test name binary filter timeout)
    add_test(NAME ${name} COMMAND ${binary} --gtest_filter=${filter})
    set_tests_properties(${name} PROPERTIES TIMEOUT ${timeout})
endfunction()

add_bug_test(bug01_bufferpool_overlap          testBugs Bug01.BufferPoolOverlapSingleThread 10)
add_bug_test(bug01_bufferpool_stress           testBugs Bug01.BufferPoolPatternIntegrityStress 30)
add_bug_test(bug01_sharedview_assign_buffers   testBugs Bug01.SharedViewAssignAcrossBuffers 10)
add_bug_test(bug01_stale_unref_during_reuse    testBugs Bug01.StaleUnrefDuringBufferReuse 30)
add_bug_test(bug02_lost_wakeup_senders         testBugsInterpose Bug02.LostWakeupConcurrentSenders 30)
add_bug_test(bug02_lost_epollout_eagain        testBugsInterpose Bug02.LostEpolloutDuringEagain 40)
add_bug_test(bug03_response_before_promise     testBugs Bug03.FastResponseDropped 40)
add_bug_test(bug04_seq_id_truncated            testBugs Bug04.SeqIdTruncated 20)
add_bug_test(bug05_peer_oversized_header       testBugs Bug05.OversizedHeaderFromPeer 20)
add_bug_test(bug05_peer_invalid_method         testBugs Bug05.InvalidMethodFromPeer 20)
add_bug_test(bug05_sender_oversized_payload    testBugs Bug05.SenderOversizedPayloadNotRejected 20)
add_bug_test(bug06_sigpipe                     testBugs Bug06.SigpipeOnResetPeer 30)
add_bug_test(bug07_close_old_conns_deadlock    testBugs Bug07.CloseOldConnsDeadlock 10)
add_bug_test(bug07_close_old_conns_count       testBugs Bug07.CloseOldConnsHonoursCount 10)
add_bug_test(bug08_protobus_dtor_hang          testBugs Bug08.ProtoBusDestructorHangs 10)
add_bug_test(bug09_flush_chains_grow           testBugs Bug09.FlushChainsGrow 20)
add_bug_test(bug10_timed_out_requests_leak     testBugs Bug10.TimedOutRequestsLeak 50)
add_bug_test(bug11_whole_second_timer          testBugsInterpose Bug11.WholeSecondDeadlineFiresImmediately 15)
add_bug_test(bug12_spin_path_runs_early        testBugs Bug12.SpinPathRunsActionEarly 20)
add_bug_test(bug13_take_available              testBugs Bug13.TakeAvailableReturnsSameConnTwice 10)
add_bug_test(bug14_close_size                  testBugs Bug14.CloseDoesNotDecrementSize 10)
add_bug_test(bug15_rebind_unknown_id           testBugs Bug15.RebindUnknownId 10)
add_bug_test(bug16_reconnect_busy_loop         testBugs Bug16.ReconnectBusyLoop 15)
add_bug_test(bug17_pool_size_race              testBugs Bug17.PoolSizeRace 40)
add_bug_test(bug18_endpoint_off_by_one         testBugs Bug18.AsyncConnectOffByOne 10)
add_bug_test(bug19_register_ipv4               testBugs Bug19.RegisterIpv4Literal 10)
add_bug_test(bug19_all_addrinfo_mapped         testBugsInterpose Bug19.AllAddrinfoResultsMapped 10)
add_bug_test(bug20_forced_bogus_endpoint       testBugs Bug20.ForcedBogusEndpoint 20)
add_bug_test(minor_sharedview_assign_leak      testBugs BugMinor.SharedViewAssignLeaksRef 10)
add_bug_test(minor_tcpbus_fd_leak              testBugs BugMinor.TcpBusLeaksFds 10)
add_bug_test(minor_inbound_nodelay             testBugs BugMinor.InboundNoDelay 10)
# sanity check of the raw-protocol test helpers; expected to PASS on current code
add_bug_test(sanity_raw_roundtrip              testBugs Sanity.RawRoundtrip 20)

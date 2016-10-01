/*
 *  Copyright (c) 2012 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include <memory>

#include "webrtc/base/array_view.h"
#include "webrtc/base/random.h"
#include "webrtc/common_types.h"
#include "webrtc/modules/rtp_rtcp/source/byte_io.h"
#include "webrtc/modules/rtp_rtcp/source/rtcp_packet.h"
#include "webrtc/modules/rtp_rtcp/source/rtcp_packet/app.h"
#include "webrtc/modules/rtp_rtcp/source/rtcp_packet/bye.h"
#include "webrtc/modules/rtp_rtcp/source/rtcp_packet/compound_packet.h"
#include "webrtc/modules/rtp_rtcp/source/rtcp_packet/extended_jitter_report.h"
#include "webrtc/modules/rtp_rtcp/source/rtcp_packet/extended_reports.h"
#include "webrtc/modules/rtp_rtcp/source/rtcp_packet/fir.h"
#include "webrtc/modules/rtp_rtcp/source/rtcp_packet/nack.h"
#include "webrtc/modules/rtp_rtcp/source/rtcp_packet/pli.h"
#include "webrtc/modules/rtp_rtcp/source/rtcp_packet/rapid_resync_request.h"
#include "webrtc/modules/rtp_rtcp/source/rtcp_packet/receiver_report.h"
#include "webrtc/modules/rtp_rtcp/source/rtcp_packet/remb.h"
#include "webrtc/modules/rtp_rtcp/source/rtcp_packet/rpsi.h"
#include "webrtc/modules/rtp_rtcp/source/rtcp_packet/sdes.h"
#include "webrtc/modules/rtp_rtcp/source/rtcp_packet/sender_report.h"
#include "webrtc/modules/rtp_rtcp/source/rtcp_packet/sli.h"
#include "webrtc/modules/rtp_rtcp/source/rtcp_packet/tmmbr.h"
#include "webrtc/modules/rtp_rtcp/source/rtcp_packet/transport_feedback.h"
#include "webrtc/modules/rtp_rtcp/source/rtcp_receiver.h"
#include "webrtc/modules/rtp_rtcp/source/time_util.h"
#include "webrtc/system_wrappers/include/ntp_time.h"
#include "webrtc/test/gmock.h"
#include "webrtc/test/gtest.h"

namespace webrtc {
namespace {

using ::testing::_;
using ::testing::AllOf;
using ::testing::ElementsAreArray;
using ::testing::Field;
using ::testing::IsEmpty;
using ::testing::NiceMock;
using ::testing::Property;
using ::testing::SizeIs;
using ::testing::StrEq;
using ::testing::StrictMock;
using ::testing::UnorderedElementsAre;

class MockRtcpPacketTypeCounterObserver : public RtcpPacketTypeCounterObserver {
 public:
  MOCK_METHOD2(RtcpPacketTypesCounterUpdated,
               void(uint32_t, const RtcpPacketTypeCounter&));
};

class MockRtcpIntraFrameObserver : public RtcpIntraFrameObserver {
 public:
  MOCK_METHOD1(OnReceivedIntraFrameRequest, void(uint32_t));
  MOCK_METHOD2(OnReceivedSLI, void(uint32_t, uint8_t));
  MOCK_METHOD2(OnReceivedRPSI, void(uint32_t, uint64_t));
  MOCK_METHOD2(OnLocalSsrcChanged, void(uint32_t, uint32_t));
};

class MockRtcpCallbackImpl : public RtcpStatisticsCallback {
 public:
  MOCK_METHOD2(StatisticsUpdated, void(const RtcpStatistics&, uint32_t));
  MOCK_METHOD2(CNameChanged, void(const char*, uint32_t));
};

class MockTransportFeedbackObserver : public TransportFeedbackObserver {
 public:
  MOCK_METHOD3(AddPacket, void(uint16_t, size_t, int));
  MOCK_METHOD1(OnTransportFeedback, void(const rtcp::TransportFeedback&));
  MOCK_CONST_METHOD0(GetTransportFeedbackVector, std::vector<PacketInfo>());
};

class MockRtcpBandwidthObserver : public RtcpBandwidthObserver {
 public:
  MOCK_METHOD1(OnReceivedEstimatedBitrate, void(uint32_t));
  MOCK_METHOD3(OnReceivedRtcpReceiverReport,
               void(const ReportBlockList&, int64_t, int64_t));
};

class MockModuleRtpRtcp : public RTCPReceiver::ModuleRtpRtcp {
 public:
  MOCK_METHOD1(SetTmmbn, void(std::vector<rtcp::TmmbItem>));
  MOCK_METHOD0(OnRequestSendReport, void());
  MOCK_METHOD1(OnReceivedNack, void(const std::vector<uint16_t>&));
  MOCK_METHOD1(OnReceivedRtcpReportBlocks, void(const ReportBlockList&));
};

// SSRC of remote peer, that sends rtcp packet to the rtcp receiver under test.
constexpr uint32_t kSenderSsrc = 0x10203;
// SSRCs of local peer, that rtcp packet addressed to.
constexpr uint32_t kReceiverMainSsrc = 0x123456;
// RtcpReceiver can accept several ssrc, e.g. regular and rtx streams.
constexpr uint32_t kReceiverExtraSsrc = 0x1234567;
// SSRCs to ignore (i.e. not configured in RtcpReceiver).
constexpr uint32_t kNotToUsSsrc = 0x654321;
constexpr uint32_t kUnknownSenderSsrc = 0x54321;

}  // namespace

class RtcpReceiverTest : public ::testing::Test {
 protected:
  RtcpReceiverTest()
      : system_clock_(1335900000),
        rtcp_receiver_(&system_clock_,
                       false,
                       &packet_type_counter_observer_,
                       &bandwidth_observer_,
                       &intra_frame_observer_,
                       &transport_feedback_observer_,
                       &rtp_rtcp_impl_) {}
  void SetUp() {
    std::set<uint32_t> ssrcs = {kReceiverMainSsrc, kReceiverExtraSsrc};
    EXPECT_CALL(intra_frame_observer_,
                OnLocalSsrcChanged(0, kReceiverMainSsrc));
    rtcp_receiver_.SetSsrcs(kReceiverMainSsrc, ssrcs);

    rtcp_receiver_.SetRemoteSSRC(kSenderSsrc);
  }

  void InjectRtcpPacket(rtc::ArrayView<const uint8_t> raw) {
    rtcp_receiver_.IncomingPacket(raw.data(), raw.size());
  }

  void InjectRtcpPacket(const rtcp::RtcpPacket& packet) {
    rtc::Buffer raw = packet.Build();
    rtcp_receiver_.IncomingPacket(raw.data(), raw.size());
  }

  SimulatedClock system_clock_;
  // Callbacks to packet_type_counter_observer are frequent but most of the time
  // are not interesting.
  NiceMock<MockRtcpPacketTypeCounterObserver> packet_type_counter_observer_;
  StrictMock<MockRtcpBandwidthObserver> bandwidth_observer_;
  StrictMock<MockRtcpIntraFrameObserver> intra_frame_observer_;
  StrictMock<MockTransportFeedbackObserver> transport_feedback_observer_;
  StrictMock<MockModuleRtpRtcp> rtp_rtcp_impl_;

  RTCPReceiver rtcp_receiver_;
};

TEST_F(RtcpReceiverTest, BrokenPacketIsIgnored) {
  const uint8_t bad_packet[] = {0, 0, 0, 0};
  EXPECT_CALL(packet_type_counter_observer_,
              RtcpPacketTypesCounterUpdated(_, _))
      .Times(0);
  InjectRtcpPacket(bad_packet);
}

TEST_F(RtcpReceiverTest, InvalidFeedbackPacketIsIgnored) {
  // Too short feedback packet.
  const uint8_t bad_packet[] = {0x81, rtcp::Rtpfb::kPacketType, 0, 0};

  // TODO(danilchap): Add expectation RtcpPacketTypesCounterUpdated
  // is not called once parser would be adjusted to avoid that callback on
  // semi-valid packets.
  InjectRtcpPacket(bad_packet);
}

TEST_F(RtcpReceiverTest, RpsiWithFractionalPaddingIsIgnored) {
  // Padding size represent fractional number of bytes.
  const uint8_t kPaddingSizeBits = 0x0b;
  // clang-format off
  const uint8_t bad_packet[] = {0x80 | rtcp::Rpsi::kFeedbackMessageType,
                                      rtcp::Rpsi::kPacketType, 0, 3,
                                0x12, 0x34, 0x56, 0x78,
                                0x98, 0x76, 0x54, 0x32,
                                kPaddingSizeBits, 0x00, 0x00, 0x00};
  // clang-format on
  EXPECT_CALL(intra_frame_observer_, OnReceivedRPSI(_, _)).Times(0);
  InjectRtcpPacket(bad_packet);
}

TEST_F(RtcpReceiverTest, RpsiWithTooLargePaddingIsIgnored) {
  // Padding size exceeds packet size.
  const uint8_t kPaddingSizeBits = 0xa8;
  // clang-format off
  const uint8_t bad_packet[] = {0x80 | rtcp::Rpsi::kFeedbackMessageType,
                                      rtcp::Rpsi::kPacketType, 0, 3,
                                0x12, 0x34, 0x56, 0x78,
                                0x98, 0x76, 0x54, 0x32,
                                kPaddingSizeBits, 0x00, 0x00, 0x00};
  // clang-format on
  EXPECT_CALL(intra_frame_observer_, OnReceivedRPSI(_, _)).Times(0);
  InjectRtcpPacket(bad_packet);
}

// With parsing using rtcp classes this test will make no sense.
// With current stateful parser this test was failing.
TEST_F(RtcpReceiverTest, TwoHalfValidRpsiAreIgnored) {
  // clang-format off
  const uint8_t bad_packet[] = {0x80 | rtcp::Rpsi::kFeedbackMessageType,
                                      rtcp::Rpsi::kPacketType, 0, 2,
                                0x12, 0x34, 0x56, 0x78,
                                0x98, 0x76, 0x54, 0x32,
                                0x80 | rtcp::Rpsi::kFeedbackMessageType,
                                      rtcp::Rpsi::kPacketType, 0, 2,
                                0x12, 0x34, 0x56, 0x78,
                                0x98, 0x76, 0x54, 0x32};
  // clang-format on
  EXPECT_CALL(intra_frame_observer_, OnReceivedRPSI(_, _)).Times(0);
  InjectRtcpPacket(bad_packet);
}

TEST_F(RtcpReceiverTest, InjectRpsiPacket) {
  const uint64_t kPictureId = 0x123456789;
  rtcp::Rpsi rpsi;
  rpsi.SetPictureId(kPictureId);

  EXPECT_CALL(intra_frame_observer_, OnReceivedRPSI(_, kPictureId));
  InjectRtcpPacket(rpsi);
}

TEST_F(RtcpReceiverTest, InjectSrPacket) {
  RTCPSenderInfo info;
  EXPECT_EQ(-1, rtcp_receiver_.SenderInfoReceived(&info));

  int64_t now = system_clock_.TimeInMilliseconds();
  rtcp::SenderReport sr;
  sr.SetSenderSsrc(kSenderSsrc);

  EXPECT_CALL(rtp_rtcp_impl_, OnReceivedRtcpReportBlocks(IsEmpty()));
  EXPECT_CALL(bandwidth_observer_,
              OnReceivedRtcpReceiverReport(IsEmpty(), _, now));
  InjectRtcpPacket(sr);

  EXPECT_EQ(0, rtcp_receiver_.SenderInfoReceived(&info));
}

TEST_F(RtcpReceiverTest, InjectSrPacketFromUnknownSender) {
  int64_t now = system_clock_.TimeInMilliseconds();
  rtcp::SenderReport sr;
  sr.SetSenderSsrc(kUnknownSenderSsrc);

  // The parser will handle report blocks in Sender Report from other than his
  // expected peer.
  EXPECT_CALL(rtp_rtcp_impl_, OnReceivedRtcpReportBlocks(_));
  EXPECT_CALL(bandwidth_observer_, OnReceivedRtcpReceiverReport(_, _, now));
  InjectRtcpPacket(sr);

  // But will not flag that he's gotten sender information.
  RTCPSenderInfo info;
  EXPECT_EQ(-1, rtcp_receiver_.SenderInfoReceived(&info));
}

TEST_F(RtcpReceiverTest, InjectSrPacketCalculatesRTT) {
  Random r(0x0123456789abcdef);
  const int64_t kRttMs = r.Rand(1, 9 * 3600 * 1000);
  const uint32_t kDelayNtp = r.Rand(0, 0x7fffffff);
  const int64_t kDelayMs = CompactNtpRttToMs(kDelayNtp);

  int64_t rtt_ms = 0;
  EXPECT_EQ(
      -1, rtcp_receiver_.RTT(kSenderSsrc, &rtt_ms, nullptr, nullptr, nullptr));

  uint32_t sent_ntp = CompactNtp(NtpTime(system_clock_));
  system_clock_.AdvanceTimeMilliseconds(kRttMs + kDelayMs);

  rtcp::SenderReport sr;
  sr.SetSenderSsrc(kSenderSsrc);
  rtcp::ReportBlock block;
  block.SetMediaSsrc(kReceiverMainSsrc);
  block.SetLastSr(sent_ntp);
  block.SetDelayLastSr(kDelayNtp);
  sr.AddReportBlock(block);

  EXPECT_CALL(rtp_rtcp_impl_, OnReceivedRtcpReportBlocks(_));
  EXPECT_CALL(bandwidth_observer_, OnReceivedRtcpReceiverReport(_, _, _));
  InjectRtcpPacket(sr);

  EXPECT_EQ(
      0, rtcp_receiver_.RTT(kSenderSsrc, &rtt_ms, nullptr, nullptr, nullptr));
  EXPECT_NEAR(kRttMs, rtt_ms, 1);
}

TEST_F(RtcpReceiverTest, InjectSrPacketCalculatesNegativeRTTAsOne) {
  Random r(0x0123456789abcdef);
  const int64_t kRttMs = r.Rand(-3600 * 1000, -1);
  const uint32_t kDelayNtp = r.Rand(0, 0x7fffffff);
  const int64_t kDelayMs = CompactNtpRttToMs(kDelayNtp);

  int64_t rtt_ms = 0;
  EXPECT_EQ(
      -1, rtcp_receiver_.RTT(kSenderSsrc, &rtt_ms, nullptr, nullptr, nullptr));

  uint32_t sent_ntp = CompactNtp(NtpTime(system_clock_));
  system_clock_.AdvanceTimeMilliseconds(kRttMs + kDelayMs);

  rtcp::SenderReport sr;
  sr.SetSenderSsrc(kSenderSsrc);
  rtcp::ReportBlock block;
  block.SetMediaSsrc(kReceiverMainSsrc);
  block.SetLastSr(sent_ntp);
  block.SetDelayLastSr(kDelayNtp);
  sr.AddReportBlock(block);

  EXPECT_CALL(rtp_rtcp_impl_, OnReceivedRtcpReportBlocks(SizeIs(1)));
  EXPECT_CALL(bandwidth_observer_,
              OnReceivedRtcpReceiverReport(SizeIs(1), _, _));
  InjectRtcpPacket(sr);

  EXPECT_EQ(
      0, rtcp_receiver_.RTT(kSenderSsrc, &rtt_ms, nullptr, nullptr, nullptr));
  EXPECT_EQ(1, rtt_ms);
}

TEST_F(RtcpReceiverTest, InjectRrPacket) {
  int64_t now = system_clock_.TimeInMilliseconds();
  rtcp::ReceiverReport rr;
  rr.SetSenderSsrc(kSenderSsrc);

  EXPECT_CALL(rtp_rtcp_impl_, OnReceivedRtcpReportBlocks(IsEmpty()));
  EXPECT_CALL(bandwidth_observer_,
              OnReceivedRtcpReceiverReport(IsEmpty(), _, now));
  InjectRtcpPacket(rr);

  RTCPSenderInfo info;
  EXPECT_EQ(-1, rtcp_receiver_.SenderInfoReceived(&info));
  EXPECT_EQ(now, rtcp_receiver_.LastReceivedReceiverReport());
  std::vector<RTCPReportBlock> report_blocks;
  rtcp_receiver_.StatisticsReceived(&report_blocks);
  EXPECT_TRUE(report_blocks.empty());
}

TEST_F(RtcpReceiverTest, InjectRrPacketWithReportBlockNotToUsIgnored) {
  int64_t now = system_clock_.TimeInMilliseconds();
  rtcp::ReportBlock rb;
  rb.SetMediaSsrc(kNotToUsSsrc);
  rtcp::ReceiverReport rr;
  rr.SetSenderSsrc(kSenderSsrc);
  rr.AddReportBlock(rb);

  EXPECT_CALL(rtp_rtcp_impl_, OnReceivedRtcpReportBlocks(IsEmpty()));
  EXPECT_CALL(bandwidth_observer_,
              OnReceivedRtcpReceiverReport(IsEmpty(), _, now));
  InjectRtcpPacket(rr);

  EXPECT_EQ(now, rtcp_receiver_.LastReceivedReceiverReport());
  std::vector<RTCPReportBlock> received_blocks;
  rtcp_receiver_.StatisticsReceived(&received_blocks);
  EXPECT_TRUE(received_blocks.empty());
}

TEST_F(RtcpReceiverTest, InjectRrPacketWithOneReportBlock) {
  int64_t now = system_clock_.TimeInMilliseconds();

  rtcp::ReportBlock rb;
  rb.SetMediaSsrc(kReceiverMainSsrc);
  rtcp::ReceiverReport rr;
  rr.SetSenderSsrc(kSenderSsrc);
  rr.AddReportBlock(rb);

  EXPECT_CALL(rtp_rtcp_impl_, OnReceivedRtcpReportBlocks(SizeIs(1)));
  EXPECT_CALL(bandwidth_observer_,
              OnReceivedRtcpReceiverReport(SizeIs(1), _, now));
  InjectRtcpPacket(rr);

  EXPECT_EQ(now, rtcp_receiver_.LastReceivedReceiverReport());
  std::vector<RTCPReportBlock> received_blocks;
  rtcp_receiver_.StatisticsReceived(&received_blocks);
  EXPECT_EQ(1u, received_blocks.size());
}

TEST_F(RtcpReceiverTest, InjectRrPacketWithTwoReportBlocks) {
  const uint16_t kSequenceNumbers[] = {10, 12423};
  const uint32_t kCumLost[] = {13, 555};
  const uint8_t kFracLost[] = {20, 11};
  int64_t now = system_clock_.TimeInMilliseconds();

  rtcp::ReportBlock rb1;
  rb1.SetMediaSsrc(kReceiverMainSsrc);
  rb1.SetExtHighestSeqNum(kSequenceNumbers[0]);
  rb1.SetFractionLost(10);

  rtcp::ReportBlock rb2;
  rb2.SetMediaSsrc(kReceiverExtraSsrc);
  rb2.SetExtHighestSeqNum(kSequenceNumbers[1]);
  rb2.SetFractionLost(0);

  rtcp::ReceiverReport rr1;
  rr1.SetSenderSsrc(kSenderSsrc);
  rr1.AddReportBlock(rb1);
  rr1.AddReportBlock(rb2);

  EXPECT_CALL(rtp_rtcp_impl_, OnReceivedRtcpReportBlocks(SizeIs(2)));
  EXPECT_CALL(bandwidth_observer_,
              OnReceivedRtcpReceiverReport(SizeIs(2), _, now));
  InjectRtcpPacket(rr1);

  EXPECT_EQ(now, rtcp_receiver_.LastReceivedReceiverReport());
  std::vector<RTCPReportBlock> received_blocks;
  rtcp_receiver_.StatisticsReceived(&received_blocks);
  EXPECT_THAT(received_blocks,
              UnorderedElementsAre(Field(&RTCPReportBlock::fractionLost, 0),
                                   Field(&RTCPReportBlock::fractionLost, 10)));

  // Insert next receiver report with same ssrc but new values.
  rtcp::ReportBlock rb3;
  rb3.SetMediaSsrc(kReceiverMainSsrc);
  rb3.SetExtHighestSeqNum(kSequenceNumbers[0]);
  rb3.SetFractionLost(kFracLost[0]);
  rb3.SetCumulativeLost(kCumLost[0]);

  rtcp::ReportBlock rb4;
  rb4.SetMediaSsrc(kReceiverExtraSsrc);
  rb4.SetExtHighestSeqNum(kSequenceNumbers[1]);
  rb4.SetFractionLost(kFracLost[1]);
  rb4.SetCumulativeLost(kCumLost[1]);

  rtcp::ReceiverReport rr2;
  rr2.SetSenderSsrc(kSenderSsrc);
  rr2.AddReportBlock(rb3);
  rr2.AddReportBlock(rb4);

  // Advance time to make 1st sent time and 2nd sent time different.
  system_clock_.AdvanceTimeMilliseconds(500);
  now = system_clock_.TimeInMilliseconds();

  EXPECT_CALL(rtp_rtcp_impl_, OnReceivedRtcpReportBlocks(SizeIs(2)));
  EXPECT_CALL(bandwidth_observer_,
              OnReceivedRtcpReceiverReport(SizeIs(2), _, now));
  InjectRtcpPacket(rr2);

  received_blocks.clear();
  rtcp_receiver_.StatisticsReceived(&received_blocks);
  EXPECT_EQ(2u, received_blocks.size());
  EXPECT_THAT(received_blocks,
              UnorderedElementsAre(
                  AllOf(Field(&RTCPReportBlock::sourceSSRC, kReceiverMainSsrc),
                        Field(&RTCPReportBlock::fractionLost, kFracLost[0]),
                        Field(&RTCPReportBlock::cumulativeLost, kCumLost[0]),
                        Field(&RTCPReportBlock::extendedHighSeqNum,
                              kSequenceNumbers[0])),
                  AllOf(Field(&RTCPReportBlock::sourceSSRC, kReceiverExtraSsrc),
                        Field(&RTCPReportBlock::fractionLost, kFracLost[1]),
                        Field(&RTCPReportBlock::cumulativeLost, kCumLost[1]),
                        Field(&RTCPReportBlock::extendedHighSeqNum,
                              kSequenceNumbers[1]))));
}

TEST_F(RtcpReceiverTest, InjectRrPacketsFromTwoRemoteSsrcs) {
  const uint32_t kSenderSsrc2 = 0x20304;
  const uint16_t kSequenceNumbers[] = {10, 12423};
  const uint32_t kCumLost[] = {13, 555};
  const uint8_t kFracLost[] = {20, 11};

  rtcp::ReportBlock rb1;
  rb1.SetMediaSsrc(kReceiverMainSsrc);
  rb1.SetExtHighestSeqNum(kSequenceNumbers[0]);
  rb1.SetFractionLost(kFracLost[0]);
  rb1.SetCumulativeLost(kCumLost[0]);
  rtcp::ReceiverReport rr1;
  rr1.SetSenderSsrc(kSenderSsrc);
  rr1.AddReportBlock(rb1);

  int64_t now = system_clock_.TimeInMilliseconds();

  EXPECT_CALL(rtp_rtcp_impl_, OnReceivedRtcpReportBlocks(SizeIs(1)));
  EXPECT_CALL(bandwidth_observer_,
              OnReceivedRtcpReceiverReport(SizeIs(1), _, now));
  InjectRtcpPacket(rr1);

  EXPECT_EQ(now, rtcp_receiver_.LastReceivedReceiverReport());

  std::vector<RTCPReportBlock> received_blocks;
  rtcp_receiver_.StatisticsReceived(&received_blocks);
  EXPECT_EQ(1u, received_blocks.size());
  EXPECT_EQ(kSenderSsrc, received_blocks[0].remoteSSRC);
  EXPECT_EQ(kReceiverMainSsrc, received_blocks[0].sourceSSRC);
  EXPECT_EQ(kFracLost[0], received_blocks[0].fractionLost);
  EXPECT_EQ(kCumLost[0], received_blocks[0].cumulativeLost);
  EXPECT_EQ(kSequenceNumbers[0], received_blocks[0].extendedHighSeqNum);

  rtcp::ReportBlock rb2;
  rb2.SetMediaSsrc(kReceiverMainSsrc);
  rb2.SetExtHighestSeqNum(kSequenceNumbers[1]);
  rb2.SetFractionLost(kFracLost[1]);
  rb2.SetCumulativeLost(kCumLost[1]);
  rtcp::ReceiverReport rr2;
  rr2.SetSenderSsrc(kSenderSsrc2);
  rr2.AddReportBlock(rb2);

  EXPECT_CALL(rtp_rtcp_impl_, OnReceivedRtcpReportBlocks(SizeIs(1)));
  EXPECT_CALL(bandwidth_observer_,
              OnReceivedRtcpReceiverReport(SizeIs(1), _, now));
  InjectRtcpPacket(rr2);

  received_blocks.clear();
  rtcp_receiver_.StatisticsReceived(&received_blocks);
  ASSERT_EQ(2u, received_blocks.size());
  EXPECT_THAT(received_blocks,
              UnorderedElementsAre(
                  AllOf(Field(&RTCPReportBlock::sourceSSRC, kReceiverMainSsrc),
                        Field(&RTCPReportBlock::remoteSSRC, kSenderSsrc),
                        Field(&RTCPReportBlock::fractionLost, kFracLost[0]),
                        Field(&RTCPReportBlock::cumulativeLost, kCumLost[0]),
                        Field(&RTCPReportBlock::extendedHighSeqNum,
                              kSequenceNumbers[0])),
                  AllOf(Field(&RTCPReportBlock::sourceSSRC, kReceiverMainSsrc),
                        Field(&RTCPReportBlock::remoteSSRC, kSenderSsrc2),
                        Field(&RTCPReportBlock::fractionLost, kFracLost[1]),
                        Field(&RTCPReportBlock::cumulativeLost, kCumLost[1]),
                        Field(&RTCPReportBlock::extendedHighSeqNum,
                              kSequenceNumbers[1]))));
}

TEST_F(RtcpReceiverTest, GetRtt) {
  // No report block received.
  EXPECT_EQ(
      -1, rtcp_receiver_.RTT(kSenderSsrc, nullptr, nullptr, nullptr, nullptr));

  rtcp::ReportBlock rb;
  rb.SetMediaSsrc(kReceiverMainSsrc);

  rtcp::ReceiverReport rr;
  rr.SetSenderSsrc(kSenderSsrc);
  rr.AddReportBlock(rb);
  int64_t now = system_clock_.TimeInMilliseconds();

  EXPECT_CALL(rtp_rtcp_impl_, OnReceivedRtcpReportBlocks(_));
  EXPECT_CALL(bandwidth_observer_, OnReceivedRtcpReceiverReport(_, _, _));
  InjectRtcpPacket(rr);

  EXPECT_EQ(now, rtcp_receiver_.LastReceivedReceiverReport());
  EXPECT_EQ(
      0, rtcp_receiver_.RTT(kSenderSsrc, nullptr, nullptr, nullptr, nullptr));
}

// Ij packets are ignored.
TEST_F(RtcpReceiverTest, InjectIjWithNoItem) {
  rtcp::ExtendedJitterReport ij;
  InjectRtcpPacket(ij);
}

// App packets are ignored.
TEST_F(RtcpReceiverTest, InjectApp) {
  rtcp::App app;
  app.SetSubType(30);
  app.SetName(0x17a177e);
  const uint8_t kData[] = {'t', 'e', 's', 't', 'd', 'a', 't', 'a'};
  app.SetData(kData, sizeof(kData));

  InjectRtcpPacket(app);
}

TEST_F(RtcpReceiverTest, InjectSdesWithOneChunk) {
  const char kCname[] = "alice@host";
  MockRtcpCallbackImpl callback;
  rtcp_receiver_.RegisterRtcpStatisticsCallback(&callback);
  rtcp::Sdes sdes;
  sdes.AddCName(kSenderSsrc, kCname);

  EXPECT_CALL(callback, CNameChanged(StrEq(kCname), kSenderSsrc));
  InjectRtcpPacket(sdes);

  char cName[RTCP_CNAME_SIZE];
  EXPECT_EQ(0, rtcp_receiver_.CNAME(kSenderSsrc, cName));
  EXPECT_EQ(0, strncmp(cName, kCname, RTCP_CNAME_SIZE));
}

TEST_F(RtcpReceiverTest, InjectByePacket_RemovesCname) {
  const char kCname[] = "alice@host";
  rtcp::Sdes sdes;
  sdes.AddCName(kSenderSsrc, kCname);

  InjectRtcpPacket(sdes);

  char cName[RTCP_CNAME_SIZE];
  EXPECT_EQ(0, rtcp_receiver_.CNAME(kSenderSsrc, cName));

  // Verify that BYE removes the CNAME.
  rtcp::Bye bye;
  bye.SetSenderSsrc(kSenderSsrc);

  InjectRtcpPacket(bye);

  EXPECT_EQ(-1, rtcp_receiver_.CNAME(kSenderSsrc, cName));
}

TEST_F(RtcpReceiverTest, InjectByePacket_RemovesReportBlocks) {
  rtcp::ReportBlock rb1;
  rb1.SetMediaSsrc(kReceiverMainSsrc);
  rtcp::ReportBlock rb2;
  rb2.SetMediaSsrc(kReceiverExtraSsrc);
  rtcp::ReceiverReport rr;
  rr.SetSenderSsrc(kSenderSsrc);
  rr.AddReportBlock(rb1);
  rr.AddReportBlock(rb2);

  EXPECT_CALL(rtp_rtcp_impl_, OnReceivedRtcpReportBlocks(_));
  EXPECT_CALL(bandwidth_observer_, OnReceivedRtcpReceiverReport(_, _, _));
  InjectRtcpPacket(rr);

  std::vector<RTCPReportBlock> received_blocks;
  rtcp_receiver_.StatisticsReceived(&received_blocks);
  EXPECT_EQ(2u, received_blocks.size());

  // Verify that BYE removes the report blocks.
  rtcp::Bye bye;
  bye.SetSenderSsrc(kSenderSsrc);

  InjectRtcpPacket(bye);

  received_blocks.clear();
  rtcp_receiver_.StatisticsReceived(&received_blocks);
  EXPECT_TRUE(received_blocks.empty());

  // Inject packet again.
  EXPECT_CALL(rtp_rtcp_impl_, OnReceivedRtcpReportBlocks(_));
  EXPECT_CALL(bandwidth_observer_, OnReceivedRtcpReceiverReport(_, _, _));
  InjectRtcpPacket(rr);

  received_blocks.clear();
  rtcp_receiver_.StatisticsReceived(&received_blocks);
  EXPECT_EQ(2u, received_blocks.size());
}

TEST_F(RtcpReceiverTest, InjectPliPacket) {
  rtcp::Pli pli;
  pli.SetMediaSsrc(kReceiverMainSsrc);

  EXPECT_CALL(
      packet_type_counter_observer_,
      RtcpPacketTypesCounterUpdated(
          kReceiverMainSsrc, Field(&RtcpPacketTypeCounter::pli_packets, 1)));
  EXPECT_CALL(intra_frame_observer_,
              OnReceivedIntraFrameRequest(kReceiverMainSsrc));
  InjectRtcpPacket(pli);
}

TEST_F(RtcpReceiverTest, PliPacketNotToUsIgnored) {
  rtcp::Pli pli;
  pli.SetMediaSsrc(kNotToUsSsrc);

  EXPECT_CALL(
      packet_type_counter_observer_,
      RtcpPacketTypesCounterUpdated(
          kReceiverMainSsrc, Field(&RtcpPacketTypeCounter::pli_packets, 0)));
  EXPECT_CALL(intra_frame_observer_, OnReceivedIntraFrameRequest(_)).Times(0);
  InjectRtcpPacket(pli);
}

TEST_F(RtcpReceiverTest, InjectFirPacket) {
  rtcp::Fir fir;
  fir.AddRequestTo(kReceiverMainSsrc, 13);

  EXPECT_CALL(
      packet_type_counter_observer_,
      RtcpPacketTypesCounterUpdated(
          kReceiverMainSsrc, Field(&RtcpPacketTypeCounter::fir_packets, 1)));
  EXPECT_CALL(intra_frame_observer_,
              OnReceivedIntraFrameRequest(kReceiverMainSsrc));
  InjectRtcpPacket(fir);
}

TEST_F(RtcpReceiverTest, FirPacketNotToUsIgnored) {
  rtcp::Fir fir;
  fir.AddRequestTo(kNotToUsSsrc, 13);

  EXPECT_CALL(intra_frame_observer_, OnReceivedIntraFrameRequest(_)).Times(0);
  InjectRtcpPacket(fir);
}

TEST_F(RtcpReceiverTest, InjectSliPacket) {
  const uint8_t kPictureId = 40;
  rtcp::Sli sli;
  sli.AddPictureId(kPictureId);

  EXPECT_CALL(intra_frame_observer_, OnReceivedSLI(_, kPictureId));
  InjectRtcpPacket(sli);
}

TEST_F(RtcpReceiverTest, ExtendedReportsPacketWithZeroReportBlocksIgnored) {
  rtcp::ExtendedReports xr;
  xr.SetSenderSsrc(kSenderSsrc);

  InjectRtcpPacket(xr);
}

// VOiP reports are ignored.
TEST_F(RtcpReceiverTest, InjectExtendedReportsVoipPacket) {
  const uint8_t kLossRate = 123;
  rtcp::VoipMetric voip_metric;
  voip_metric.SetMediaSsrc(kReceiverMainSsrc);
  RTCPVoIPMetric metric;
  metric.lossRate = kLossRate;
  voip_metric.SetVoipMetric(metric);
  rtcp::ExtendedReports xr;
  xr.SetSenderSsrc(kSenderSsrc);
  xr.AddVoipMetric(voip_metric);

  InjectRtcpPacket(xr);
}

TEST_F(RtcpReceiverTest, ExtendedReportsVoipPacketNotToUsIgnored) {
  rtcp::VoipMetric voip_metric;
  voip_metric.SetMediaSsrc(kNotToUsSsrc);
  rtcp::ExtendedReports xr;
  xr.SetSenderSsrc(kSenderSsrc);
  xr.AddVoipMetric(voip_metric);

  InjectRtcpPacket(xr);
}

TEST_F(RtcpReceiverTest, InjectExtendedReportsReceiverReferenceTimePacket) {
  const NtpTime kNtp(0x10203, 0x40506);
  rtcp::Rrtr rrtr;
  rrtr.SetNtp(kNtp);
  rtcp::ExtendedReports xr;
  xr.SetSenderSsrc(kSenderSsrc);
  xr.AddRrtr(rrtr);

  rtcp::ReceiveTimeInfo rrtime;
  EXPECT_FALSE(rtcp_receiver_.LastReceivedXrReferenceTimeInfo(&rrtime));

  InjectRtcpPacket(xr);

  EXPECT_TRUE(rtcp_receiver_.LastReceivedXrReferenceTimeInfo(&rrtime));
  EXPECT_EQ(rrtime.ssrc, kSenderSsrc);
  EXPECT_EQ(rrtime.last_rr, CompactNtp(kNtp));
  EXPECT_EQ(0U, rrtime.delay_since_last_rr);

  system_clock_.AdvanceTimeMilliseconds(1500);
  EXPECT_TRUE(rtcp_receiver_.LastReceivedXrReferenceTimeInfo(&rrtime));
  EXPECT_NEAR(1500, CompactNtpRttToMs(rrtime.delay_since_last_rr), 1);
}

TEST_F(RtcpReceiverTest, ExtendedReportsDlrrPacketNotToUsIgnored) {
  // Allow calculate rtt using dlrr/rrtr, simulating media receiver side.
  rtcp_receiver_.SetRtcpXrRrtrStatus(true);

  rtcp::Dlrr dlrr;
  dlrr.AddDlrrItem(kNotToUsSsrc, 0x12345, 0x67890);
  rtcp::ExtendedReports xr;
  xr.SetSenderSsrc(kSenderSsrc);
  xr.AddDlrr(dlrr);

  InjectRtcpPacket(xr);

  int64_t rtt_ms = 0;
  EXPECT_FALSE(rtcp_receiver_.GetAndResetXrRrRtt(&rtt_ms));
}

TEST_F(RtcpReceiverTest, InjectExtendedReportsDlrrPacketWithSubBlock) {
  const uint32_t kLastRR = 0x12345;
  const uint32_t kDelay = 0x23456;
  rtcp_receiver_.SetRtcpXrRrtrStatus(true);
  int64_t rtt_ms = 0;
  EXPECT_FALSE(rtcp_receiver_.GetAndResetXrRrRtt(&rtt_ms));

  rtcp::Dlrr dlrr;
  dlrr.AddDlrrItem(kReceiverMainSsrc, kLastRR, kDelay);
  rtcp::ExtendedReports xr;
  xr.SetSenderSsrc(kSenderSsrc);
  xr.AddDlrr(dlrr);

  InjectRtcpPacket(xr);

  uint32_t compact_ntp_now = CompactNtp(NtpTime(system_clock_));
  EXPECT_TRUE(rtcp_receiver_.GetAndResetXrRrRtt(&rtt_ms));
  uint32_t rtt_ntp = compact_ntp_now - kDelay - kLastRR;
  EXPECT_NEAR(CompactNtpRttToMs(rtt_ntp), rtt_ms, 1);
}

TEST_F(RtcpReceiverTest, InjectExtendedReportsDlrrPacketWithMultipleSubBlocks) {
  const uint32_t kLastRR = 0x12345;
  const uint32_t kDelay = 0x56789;
  rtcp_receiver_.SetRtcpXrRrtrStatus(true);

  rtcp::ExtendedReports xr;
  xr.SetSenderSsrc(kSenderSsrc);
  rtcp::Dlrr dlrr;
  dlrr.AddDlrrItem(kReceiverMainSsrc, kLastRR, kDelay);
  dlrr.AddDlrrItem(kReceiverMainSsrc + 1, 0x12345, 0x67890);
  dlrr.AddDlrrItem(kReceiverMainSsrc + 2, 0x12345, 0x67890);
  xr.AddDlrr(dlrr);

  InjectRtcpPacket(xr);

  uint32_t compact_ntp_now = CompactNtp(NtpTime(system_clock_));
  int64_t rtt_ms = 0;
  EXPECT_TRUE(rtcp_receiver_.GetAndResetXrRrRtt(&rtt_ms));
  uint32_t rtt_ntp = compact_ntp_now - kDelay - kLastRR;
  EXPECT_NEAR(CompactNtpRttToMs(rtt_ntp), rtt_ms, 1);
}

TEST_F(RtcpReceiverTest, InjectExtendedReportsPacketWithMultipleReportBlocks) {
  rtcp_receiver_.SetRtcpXrRrtrStatus(true);

  rtcp::Rrtr rrtr;
  rtcp::Dlrr dlrr;
  dlrr.AddDlrrItem(kReceiverMainSsrc, 0x12345, 0x67890);
  rtcp::VoipMetric metric;
  metric.SetMediaSsrc(kReceiverMainSsrc);
  rtcp::ExtendedReports xr;
  xr.SetSenderSsrc(kSenderSsrc);
  xr.AddRrtr(rrtr);
  xr.AddDlrr(dlrr);
  xr.AddVoipMetric(metric);

  InjectRtcpPacket(xr);

  rtcp::ReceiveTimeInfo rrtime;
  EXPECT_TRUE(rtcp_receiver_.LastReceivedXrReferenceTimeInfo(&rrtime));
  int64_t rtt_ms = 0;
  EXPECT_TRUE(rtcp_receiver_.GetAndResetXrRrRtt(&rtt_ms));
}

TEST_F(RtcpReceiverTest, InjectExtendedReportsPacketWithUnknownReportBlock) {
  rtcp_receiver_.SetRtcpXrRrtrStatus(true);

  rtcp::Rrtr rrtr;
  rtcp::Dlrr dlrr;
  dlrr.AddDlrrItem(kReceiverMainSsrc, 0x12345, 0x67890);
  rtcp::VoipMetric metric;
  metric.SetMediaSsrc(kReceiverMainSsrc);
  rtcp::ExtendedReports xr;
  xr.SetSenderSsrc(kSenderSsrc);
  xr.AddRrtr(rrtr);
  xr.AddDlrr(dlrr);
  xr.AddVoipMetric(metric);

  rtc::Buffer packet = xr.Build();
  // Modify the DLRR block to have an unsupported block type, from 5 to 6.
  ASSERT_EQ(5, packet.data()[20]);
  packet.data()[20] = 6;
  InjectRtcpPacket(packet);

  // Validate Rrtr was received and processed.
  rtcp::ReceiveTimeInfo rrtime;
  EXPECT_TRUE(rtcp_receiver_.LastReceivedXrReferenceTimeInfo(&rrtime));
  // Validate Dlrr report wasn't processed.
  int64_t rtt_ms = 0;
  EXPECT_FALSE(rtcp_receiver_.GetAndResetXrRrRtt(&rtt_ms));
}

TEST_F(RtcpReceiverTest, TestExtendedReportsRrRttInitiallyFalse) {
  rtcp_receiver_.SetRtcpXrRrtrStatus(true);

  int64_t rtt_ms;
  EXPECT_FALSE(rtcp_receiver_.GetAndResetXrRrRtt(&rtt_ms));
}

TEST_F(RtcpReceiverTest, RttCalculatedAfterExtendedReportsDlrr) {
  Random rand(0x0123456789abcdef);
  const int64_t kRttMs = rand.Rand(1, 9 * 3600 * 1000);
  const uint32_t kDelayNtp = rand.Rand(0, 0x7fffffff);
  const int64_t kDelayMs = CompactNtpRttToMs(kDelayNtp);
  rtcp_receiver_.SetRtcpXrRrtrStatus(true);
  NtpTime now(system_clock_);
  uint32_t sent_ntp = CompactNtp(now);
  system_clock_.AdvanceTimeMilliseconds(kRttMs + kDelayMs);

  rtcp::Dlrr dlrr;
  dlrr.AddDlrrItem(kReceiverMainSsrc, sent_ntp, kDelayNtp);
  rtcp::ExtendedReports xr;
  xr.SetSenderSsrc(kSenderSsrc);
  xr.AddDlrr(dlrr);

  InjectRtcpPacket(xr);

  int64_t rtt_ms = 0;
  EXPECT_TRUE(rtcp_receiver_.GetAndResetXrRrRtt(&rtt_ms));
  EXPECT_NEAR(kRttMs, rtt_ms, 1);
}

TEST_F(RtcpReceiverTest, XrDlrrCalculatesNegativeRttAsOne) {
  Random rand(0x0123456789abcdef);
  const int64_t kRttMs = rand.Rand(-3600 * 1000, -1);
  const uint32_t kDelayNtp = rand.Rand(0, 0x7fffffff);
  const int64_t kDelayMs = CompactNtpRttToMs(kDelayNtp);
  NtpTime now(system_clock_);
  uint32_t sent_ntp = CompactNtp(now);
  system_clock_.AdvanceTimeMilliseconds(kRttMs + kDelayMs);
  rtcp_receiver_.SetRtcpXrRrtrStatus(true);

  rtcp::Dlrr dlrr;
  dlrr.AddDlrrItem(kReceiverMainSsrc, sent_ntp, kDelayNtp);
  rtcp::ExtendedReports xr;
  xr.SetSenderSsrc(kSenderSsrc);
  xr.AddDlrr(dlrr);

  InjectRtcpPacket(xr);

  int64_t rtt_ms = 0;
  EXPECT_TRUE(rtcp_receiver_.GetAndResetXrRrRtt(&rtt_ms));
  EXPECT_EQ(1, rtt_ms);
}

TEST_F(RtcpReceiverTest, LastReceivedXrReferenceTimeInfoInitiallyFalse) {
  rtcp::ReceiveTimeInfo info;
  EXPECT_FALSE(rtcp_receiver_.LastReceivedXrReferenceTimeInfo(&info));
}

TEST_F(RtcpReceiverTest, GetLastReceivedExtendedReportsReferenceTimeInfo) {
  const NtpTime kNtp(0x10203, 0x40506);
  const uint32_t kNtpMid = CompactNtp(kNtp);

  rtcp::Rrtr rrtr;
  rrtr.SetNtp(kNtp);
  rtcp::ExtendedReports xr;
  xr.SetSenderSsrc(kSenderSsrc);
  xr.AddRrtr(rrtr);

  InjectRtcpPacket(xr);

  rtcp::ReceiveTimeInfo info;
  EXPECT_TRUE(rtcp_receiver_.LastReceivedXrReferenceTimeInfo(&info));
  EXPECT_EQ(kSenderSsrc, info.ssrc);
  EXPECT_EQ(kNtpMid, info.last_rr);
  EXPECT_EQ(0U, info.delay_since_last_rr);

  system_clock_.AdvanceTimeMilliseconds(1000);
  EXPECT_TRUE(rtcp_receiver_.LastReceivedXrReferenceTimeInfo(&info));
  EXPECT_EQ(65536U, info.delay_since_last_rr);
}

TEST_F(RtcpReceiverTest, ReceiveReportTimeout) {
  const int64_t kRtcpIntervalMs = 1000;
  const uint16_t kSequenceNumber = 1234;
  system_clock_.AdvanceTimeMilliseconds(3 * kRtcpIntervalMs);

  // No RR received, shouldn't trigger a timeout.
  EXPECT_FALSE(rtcp_receiver_.RtcpRrTimeout(kRtcpIntervalMs));
  EXPECT_FALSE(rtcp_receiver_.RtcpRrSequenceNumberTimeout(kRtcpIntervalMs));

  // Add a RR and advance the clock just enough to not trigger a timeout.
  rtcp::ReportBlock rb1;
  rb1.SetMediaSsrc(kReceiverMainSsrc);
  rb1.SetExtHighestSeqNum(kSequenceNumber);
  rtcp::ReceiverReport rr1;
  rr1.SetSenderSsrc(kSenderSsrc);
  rr1.AddReportBlock(rb1);

  EXPECT_CALL(rtp_rtcp_impl_, OnReceivedRtcpReportBlocks(_));
  EXPECT_CALL(bandwidth_observer_, OnReceivedRtcpReceiverReport(_, _, _));
  InjectRtcpPacket(rr1);

  system_clock_.AdvanceTimeMilliseconds(3 * kRtcpIntervalMs - 1);
  EXPECT_FALSE(rtcp_receiver_.RtcpRrTimeout(kRtcpIntervalMs));
  EXPECT_FALSE(rtcp_receiver_.RtcpRrSequenceNumberTimeout(kRtcpIntervalMs));

  // Add a RR with the same extended max as the previous RR to trigger a
  // sequence number timeout, but not a RR timeout.
  EXPECT_CALL(rtp_rtcp_impl_, OnReceivedRtcpReportBlocks(_));
  EXPECT_CALL(bandwidth_observer_, OnReceivedRtcpReceiverReport(_, _, _));
  InjectRtcpPacket(rr1);

  system_clock_.AdvanceTimeMilliseconds(2);
  EXPECT_FALSE(rtcp_receiver_.RtcpRrTimeout(kRtcpIntervalMs));
  EXPECT_TRUE(rtcp_receiver_.RtcpRrSequenceNumberTimeout(kRtcpIntervalMs));

  // Advance clock enough to trigger an RR timeout too.
  system_clock_.AdvanceTimeMilliseconds(3 * kRtcpIntervalMs);
  EXPECT_TRUE(rtcp_receiver_.RtcpRrTimeout(kRtcpIntervalMs));

  // We should only get one timeout even though we still haven't received a new
  // RR.
  EXPECT_FALSE(rtcp_receiver_.RtcpRrTimeout(kRtcpIntervalMs));
  EXPECT_FALSE(rtcp_receiver_.RtcpRrSequenceNumberTimeout(kRtcpIntervalMs));

  // Add a new RR with increase sequence number to reset timers.
  rtcp::ReportBlock rb2;
  rb2.SetMediaSsrc(kReceiverMainSsrc);
  rb2.SetExtHighestSeqNum(kSequenceNumber + 1);
  rtcp::ReceiverReport rr2;
  rr2.SetSenderSsrc(kSenderSsrc);
  rr2.AddReportBlock(rb2);

  EXPECT_CALL(rtp_rtcp_impl_, OnReceivedRtcpReportBlocks(_));
  EXPECT_CALL(bandwidth_observer_, OnReceivedRtcpReceiverReport(_, _, _));
  InjectRtcpPacket(rr2);

  EXPECT_FALSE(rtcp_receiver_.RtcpRrTimeout(kRtcpIntervalMs));
  EXPECT_FALSE(rtcp_receiver_.RtcpRrSequenceNumberTimeout(kRtcpIntervalMs));

  // Verify we can get a timeout again once we've received new RR.
  system_clock_.AdvanceTimeMilliseconds(2 * kRtcpIntervalMs);
  EXPECT_CALL(rtp_rtcp_impl_, OnReceivedRtcpReportBlocks(_));
  EXPECT_CALL(bandwidth_observer_, OnReceivedRtcpReceiverReport(_, _, _));
  InjectRtcpPacket(rr2);

  system_clock_.AdvanceTimeMilliseconds(kRtcpIntervalMs + 1);
  EXPECT_FALSE(rtcp_receiver_.RtcpRrTimeout(kRtcpIntervalMs));
  EXPECT_TRUE(rtcp_receiver_.RtcpRrSequenceNumberTimeout(kRtcpIntervalMs));

  system_clock_.AdvanceTimeMilliseconds(2 * kRtcpIntervalMs);
  EXPECT_TRUE(rtcp_receiver_.RtcpRrTimeout(kRtcpIntervalMs));
}

TEST_F(RtcpReceiverTest, TmmbrReceivedWithNoIncomingPacket) {
  EXPECT_EQ(0u, rtcp_receiver_.TmmbrReceived().size());
}

TEST_F(RtcpReceiverTest, TmmbrPacketAccepted) {
  const uint32_t kBitrateBps = 30000;
  rtcp::Tmmbr tmmbr;
  tmmbr.SetSenderSsrc(kSenderSsrc);
  tmmbr.AddTmmbr(rtcp::TmmbItem(kReceiverMainSsrc, kBitrateBps, 0));
  rtcp::SenderReport sr;
  sr.SetSenderSsrc(kSenderSsrc);
  rtcp::CompoundPacket compound;
  compound.Append(&sr);
  compound.Append(&tmmbr);

  EXPECT_CALL(rtp_rtcp_impl_, OnReceivedRtcpReportBlocks(_));
  EXPECT_CALL(rtp_rtcp_impl_, SetTmmbn(SizeIs(1)));
  EXPECT_CALL(bandwidth_observer_, OnReceivedRtcpReceiverReport(_, _, _));
  EXPECT_CALL(bandwidth_observer_, OnReceivedEstimatedBitrate(kBitrateBps));
  InjectRtcpPacket(compound);

  std::vector<rtcp::TmmbItem> tmmbr_received = rtcp_receiver_.TmmbrReceived();
  ASSERT_EQ(1u, tmmbr_received.size());
  EXPECT_EQ(kBitrateBps, tmmbr_received[0].bitrate_bps());
  EXPECT_EQ(kSenderSsrc, tmmbr_received[0].ssrc());
}

TEST_F(RtcpReceiverTest, TmmbrPacketNotForUsIgnored) {
  const uint32_t kBitrateBps = 30000;
  rtcp::Tmmbr tmmbr;
  tmmbr.SetSenderSsrc(kSenderSsrc);
  tmmbr.AddTmmbr(rtcp::TmmbItem(kNotToUsSsrc, kBitrateBps, 0));

  rtcp::SenderReport sr;
  sr.SetSenderSsrc(kSenderSsrc);
  rtcp::CompoundPacket compound;
  compound.Append(&sr);
  compound.Append(&tmmbr);

  EXPECT_CALL(rtp_rtcp_impl_, OnReceivedRtcpReportBlocks(_));
  EXPECT_CALL(bandwidth_observer_, OnReceivedRtcpReceiverReport(_, _, _));
  EXPECT_CALL(bandwidth_observer_, OnReceivedEstimatedBitrate(_)).Times(0);
  InjectRtcpPacket(compound);

  EXPECT_EQ(0u, rtcp_receiver_.TmmbrReceived().size());
}

TEST_F(RtcpReceiverTest, TmmbrPacketZeroRateIgnored) {
  rtcp::Tmmbr tmmbr;
  tmmbr.SetSenderSsrc(kSenderSsrc);
  tmmbr.AddTmmbr(rtcp::TmmbItem(kReceiverMainSsrc, 0, 0));
  rtcp::SenderReport sr;
  sr.SetSenderSsrc(kSenderSsrc);
  rtcp::CompoundPacket compound;
  compound.Append(&sr);
  compound.Append(&tmmbr);

  EXPECT_CALL(rtp_rtcp_impl_, OnReceivedRtcpReportBlocks(_));
  EXPECT_CALL(bandwidth_observer_, OnReceivedRtcpReceiverReport(_, _, _));
  EXPECT_CALL(bandwidth_observer_, OnReceivedEstimatedBitrate(_)).Times(0);
  InjectRtcpPacket(compound);

  EXPECT_EQ(0u, rtcp_receiver_.TmmbrReceived().size());
}

TEST_F(RtcpReceiverTest, TmmbrThreeConstraintsTimeOut) {
  // Inject 3 packets "from" kSenderSsrc, kSenderSsrc+1, kSenderSsrc+2.
  // The times of arrival are starttime + 0, starttime + 5 and starttime + 10.
  for (uint32_t ssrc = kSenderSsrc; ssrc < kSenderSsrc + 3; ++ssrc) {
    rtcp::Tmmbr tmmbr;
    tmmbr.SetSenderSsrc(ssrc);
    tmmbr.AddTmmbr(rtcp::TmmbItem(kReceiverMainSsrc, 30000, 0));
    rtcp::SenderReport sr;
    sr.SetSenderSsrc(ssrc);
    rtcp::CompoundPacket compound;
    compound.Append(&sr);
    compound.Append(&tmmbr);

    EXPECT_CALL(rtp_rtcp_impl_, OnReceivedRtcpReportBlocks(_));
    EXPECT_CALL(rtp_rtcp_impl_, SetTmmbn(_));
    EXPECT_CALL(bandwidth_observer_, OnReceivedRtcpReceiverReport(_, _, _));
    EXPECT_CALL(bandwidth_observer_, OnReceivedEstimatedBitrate(_));
    InjectRtcpPacket(compound);

    // 5 seconds between each packet.
    system_clock_.AdvanceTimeMilliseconds(5000);
  }
  // It is now starttime + 15.
  std::vector<rtcp::TmmbItem> candidate_set = rtcp_receiver_.TmmbrReceived();
  ASSERT_EQ(3u, candidate_set.size());
  EXPECT_EQ(30000U, candidate_set[0].bitrate_bps());

  // We expect the timeout to be 25 seconds. Advance the clock by 12
  // seconds, timing out the first packet.
  system_clock_.AdvanceTimeMilliseconds(12000);
  candidate_set = rtcp_receiver_.TmmbrReceived();
  ASSERT_EQ(2u, candidate_set.size());
  EXPECT_EQ(kSenderSsrc + 1, candidate_set[0].ssrc());
}

TEST_F(RtcpReceiverTest, Callbacks) {
  MockRtcpCallbackImpl callback;
  rtcp_receiver_.RegisterRtcpStatisticsCallback(&callback);

  const uint8_t kFractionLoss = 3;
  const uint32_t kCumulativeLoss = 7;
  const uint32_t kJitter = 9;
  const uint16_t kSequenceNumber = 1234;

  // First packet, all numbers should just propagate.
  rtcp::ReportBlock rb1;
  rb1.SetMediaSsrc(kReceiverMainSsrc);
  rb1.SetExtHighestSeqNum(kSequenceNumber);
  rb1.SetFractionLost(kFractionLoss);
  rb1.SetCumulativeLost(kCumulativeLoss);
  rb1.SetJitter(kJitter);

  rtcp::ReceiverReport rr1;
  rr1.SetSenderSsrc(kSenderSsrc);
  rr1.AddReportBlock(rb1);
  EXPECT_CALL(
      callback,
      StatisticsUpdated(
          AllOf(Field(&RtcpStatistics::fraction_lost, kFractionLoss),
                Field(&RtcpStatistics::cumulative_lost, kCumulativeLoss),
                Field(&RtcpStatistics::extended_max_sequence_number,
                      kSequenceNumber),
                Field(&RtcpStatistics::jitter, kJitter)),
          kReceiverMainSsrc));
  EXPECT_CALL(rtp_rtcp_impl_, OnReceivedRtcpReportBlocks(_));
  EXPECT_CALL(bandwidth_observer_, OnReceivedRtcpReceiverReport(_, _, _));
  InjectRtcpPacket(rr1);

  rtcp_receiver_.RegisterRtcpStatisticsCallback(nullptr);

  // Add arbitrary numbers, callback should not be called.
  rtcp::ReportBlock rb2;
  rb2.SetMediaSsrc(kReceiverMainSsrc);
  rb2.SetExtHighestSeqNum(kSequenceNumber + 1);
  rb2.SetFractionLost(42);
  rb2.SetCumulativeLost(137);
  rb2.SetJitter(4711);

  rtcp::ReceiverReport rr2;
  rr2.SetSenderSsrc(kSenderSsrc);
  rr2.AddReportBlock(rb2);

  EXPECT_CALL(rtp_rtcp_impl_, OnReceivedRtcpReportBlocks(_));
  EXPECT_CALL(bandwidth_observer_, OnReceivedRtcpReceiverReport(_, _, _));
  EXPECT_CALL(callback, StatisticsUpdated(_, _)).Times(0);
  InjectRtcpPacket(rr2);
}

TEST_F(RtcpReceiverTest, ReceivesTransportFeedback) {
  rtcp::TransportFeedback packet;
  packet.SetMediaSsrc(kReceiverMainSsrc);
  packet.SetSenderSsrc(kSenderSsrc);
  packet.SetBase(1, 1000);
  packet.AddReceivedPacket(1, 1000);

  EXPECT_CALL(
      transport_feedback_observer_,
      OnTransportFeedback(AllOf(
          Property(&rtcp::TransportFeedback::media_ssrc, kReceiverMainSsrc),
          Property(&rtcp::TransportFeedback::sender_ssrc, kSenderSsrc))));
  InjectRtcpPacket(packet);
}

TEST_F(RtcpReceiverTest, ReceivesRemb) {
  const uint32_t kBitrateBps = 500000;
  rtcp::Remb remb;
  remb.SetSenderSsrc(kSenderSsrc);
  remb.SetBitrateBps(kBitrateBps);

  EXPECT_CALL(bandwidth_observer_, OnReceivedEstimatedBitrate(kBitrateBps));
  InjectRtcpPacket(remb);
}

TEST_F(RtcpReceiverTest, HandlesInvalidTransportFeedback) {
  // Send a compound packet with a TransportFeedback followed by something else.
  rtcp::TransportFeedback packet;
  packet.SetMediaSsrc(kReceiverMainSsrc);
  packet.SetSenderSsrc(kSenderSsrc);
  packet.SetBase(1, 1000);
  packet.AddReceivedPacket(1, 1000);

  static uint32_t kBitrateBps = 50000;
  rtcp::Remb remb;
  remb.SetSenderSsrc(kSenderSsrc);
  remb.SetBitrateBps(kBitrateBps);
  rtcp::CompoundPacket compound;
  compound.Append(&packet);
  compound.Append(&remb);
  rtc::Buffer built_packet = compound.Build();

  // Modify the TransportFeedback packet so that it is invalid.
  const size_t kStatusCountOffset = 14;
  ByteWriter<uint16_t>::WriteBigEndian(
      &built_packet.data()[kStatusCountOffset], 42);

  // Stress no transport feedback is expected.
  EXPECT_CALL(transport_feedback_observer_, OnTransportFeedback(_)).Times(0);
  // But remb should be processed and cause a callback
  EXPECT_CALL(bandwidth_observer_, OnReceivedEstimatedBitrate(kBitrateBps));
  InjectRtcpPacket(built_packet);
}

TEST_F(RtcpReceiverTest, Nack) {
  const uint16_t kNackList1[] = {1, 2, 3, 5};
  const size_t kNackListLength1 = std::end(kNackList1) - std::begin(kNackList1);
  const uint16_t kNackList2[] = {5, 7, 30, 40};
  const size_t kNackListLength2 = std::end(kNackList2) - std::begin(kNackList2);
  std::set<uint16_t> nack_set;
  nack_set.insert(std::begin(kNackList1), std::end(kNackList1));
  nack_set.insert(std::begin(kNackList2), std::end(kNackList2));

  rtcp::Nack nack;
  nack.SetSenderSsrc(kSenderSsrc);
  nack.SetMediaSsrc(kReceiverMainSsrc);
  nack.SetPacketIds(kNackList1, kNackListLength1);

  EXPECT_CALL(rtp_rtcp_impl_, OnReceivedNack(ElementsAreArray(kNackList1)));
  EXPECT_CALL(
      packet_type_counter_observer_,
      RtcpPacketTypesCounterUpdated(
          kReceiverMainSsrc,
          AllOf(Field(&RtcpPacketTypeCounter::nack_requests, kNackListLength1),
                Field(&RtcpPacketTypeCounter::unique_nack_requests,
                      kNackListLength1))));
  InjectRtcpPacket(nack);

  rtcp::Nack nack2;
  nack2.SetSenderSsrc(kSenderSsrc);
  nack2.SetMediaSsrc(kReceiverMainSsrc);
  nack2.SetPacketIds(kNackList2, kNackListLength2);
  EXPECT_CALL(rtp_rtcp_impl_, OnReceivedNack(ElementsAreArray(kNackList2)));
  EXPECT_CALL(packet_type_counter_observer_,
              RtcpPacketTypesCounterUpdated(
                  kReceiverMainSsrc,
                  AllOf(Field(&RtcpPacketTypeCounter::nack_requests,
                              kNackListLength1 + kNackListLength2),
                        Field(&RtcpPacketTypeCounter::unique_nack_requests,
                              nack_set.size()))));
  InjectRtcpPacket(nack2);
}

TEST_F(RtcpReceiverTest, NackNotForUsIgnored) {
  const uint16_t kNackList1[] = {1, 2, 3, 5};
  const size_t kNackListLength1 = std::end(kNackList1) - std::begin(kNackList1);

  rtcp::Nack nack;
  nack.SetSenderSsrc(kSenderSsrc);
  nack.SetMediaSsrc(kNotToUsSsrc);
  nack.SetPacketIds(kNackList1, kNackListLength1);

  EXPECT_CALL(packet_type_counter_observer_,
              RtcpPacketTypesCounterUpdated(
                  _, Field(&RtcpPacketTypeCounter::nack_requests, 0)));
  InjectRtcpPacket(nack);
}

TEST_F(RtcpReceiverTest, ForceSenderReport) {
  rtcp::RapidResyncRequest rr;
  rr.SetSenderSsrc(kSenderSsrc);
  rr.SetMediaSsrc(kReceiverMainSsrc);

  EXPECT_CALL(rtp_rtcp_impl_, OnRequestSendReport());
  InjectRtcpPacket(rr);
}

}  // namespace webrtc

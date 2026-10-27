/**
 * @file tests/unit/test_foveation.cpp
 * @brief Tests for gaze packet reception and stale-state handling.
 */
// test includes
#include "../tests_common.h"

// standard includes
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <thread>
#include <vector>

// lib includes
#include <boost/asio.hpp>
#include <flatbuffers/flatbuffers.h>

// local includes
#include <gaze_packet_generated.h>
#include <src/foveation.h>

using namespace std::literals;

namespace {
  std::vector<std::uint8_t> make_gaze_packet(std::uint64_t sequence, bool valid = true, std::uint8_t confidence = 3) {
    flatbuffers::FlatBufferBuilder builder;
    auto root = Sunshine::Foveation::CreateGazePacket(builder, 1, sequence, 12.5, 0.5f, 0.5f, 0.08f, 0.22f, 0.36f, 0.5f, confidence, valid);
    Sunshine::Foveation::FinishGazePacketBuffer(builder, root);
    return std::vector<std::uint8_t>(builder.GetBufferPointer(), builder.GetBufferPointer() + builder.GetSize());
  }

  void send_packet(const std::vector<std::uint8_t> &packet) {
    boost::asio::io_context io;
    boost::asio::ip::udp::socket socket(io);
    socket.open(boost::asio::ip::udp::v4());
    const auto *configured_port = std::getenv("SUNSHINE_GAZE_PORT");
    const auto port = configured_port ? static_cast<std::uint16_t>(std::strtoul(configured_port, nullptr, 10)) : foveation::DEFAULT_GAZE_PORT;
    socket.send_to(boost::asio::buffer(packet), {boost::asio::ip::address_v4::loopback(), port});
    std::this_thread::sleep_for(30ms);
  }
}  // namespace

TEST(FoveationTest, ParsesOptionalBlurTintFromEnvironment) {
  setenv("SUNSHINE_GAZE_BLUR_COLOR", "#ff4080", 1);
  const auto color = foveation::blur_color();

  ASSERT_TRUE(color.has_value());
  EXPECT_FLOAT_EQ((*color)[0], 1.0f);
  EXPECT_NEAR((*color)[1], 64.0f / 255.0f, 0.0001f);
  EXPECT_NEAR((*color)[2], 128.0f / 255.0f, 0.0001f);
}

TEST(FoveationTest, PublishesNewestValidGazeAndExpiresStaleState) {
  foveation::start_receiver();
  send_packet(make_gaze_packet(1));

  auto gaze = foveation::latest_state();
  ASSERT_TRUE(gaze.has_value());
  EXPECT_EQ(gaze->sequence, 1);
  EXPECT_FLOAT_EQ(gaze->center_x, 0.5f);
  EXPECT_FLOAT_EQ(gaze->center_y, 0.5f);
  EXPECT_EQ(gaze->confidence, 3);

  send_packet(make_gaze_packet(2, false));
  EXPECT_FALSE(foveation::latest_state().has_value());

  // Reordered packets must not replace the more recent invalid state.
  send_packet(make_gaze_packet(1));
  EXPECT_FALSE(foveation::latest_state().has_value());

  const std::array<std::uint8_t, 4> malformed {0, 1, 2, 3};
  send_packet(std::vector<std::uint8_t>(malformed.begin(), malformed.end()));
  EXPECT_FALSE(foveation::latest_state().has_value());

  send_packet(make_gaze_packet(3, true, 0));
  EXPECT_FALSE(foveation::latest_state().has_value());

  send_packet(make_gaze_packet(4));
  ASSERT_TRUE(foveation::latest_state().has_value());
  std::this_thread::sleep_for(300ms);
  EXPECT_FALSE(foveation::latest_state().has_value());
}

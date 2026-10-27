/**
 * @file src/foveation.cpp
 * @brief UDP gaze receiver and fresh-state publication for GPU foveation.
 */
// standard includes
#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <locale>
#include <mutex>
#include <optional>
#include <string_view>
#include <thread>

// lib includes
#include <boost/asio.hpp>
#include <flatbuffers/flatbuffers.h>
#include <flatbuffers/verifier.h>

// local includes
#include "foveation.h"
#include "gaze_packet_generated.h"
#include "logging.h"
#include "platform/common.h"

using namespace std::literals;

namespace foveation {
  namespace {
    using udp = boost::asio::ip::udp;

    constexpr auto GAZE_TIMEOUT = 250ms;

    /**
     * @brief Read the configured UDP gaze port from the process environment.
     *
     * @return A valid configured port, or the default gaze port.
     */
    std::uint16_t gaze_port() {
      const auto *value = std::getenv("SUNSHINE_GAZE_PORT");
      if (!value || !*value) {
        return DEFAULT_GAZE_PORT;
      }

      unsigned int port {};
      const auto length = std::strlen(value);
      const auto result = std::from_chars(value, value + length, port);
      if (result.ec != std::errc {} || result.ptr != value + length || port == 0 || port > 65535) {
        BOOST_LOG(warning) << "Invalid SUNSHINE_GAZE_PORT value [" << value << "]; using [" << DEFAULT_GAZE_PORT << ']';
        return DEFAULT_GAZE_PORT;
      }
      return static_cast<std::uint16_t>(port);
    }

    struct received_state_t: state_t {
      bool valid {};
      std::chrono::steady_clock::time_point received_at {};
    };

    std::mutex state_mutex;
    received_state_t latest_state;
    bool has_sequence = false;

    /**
     * @brief Append a receiver-side telemetry record for one UDP datagram.
     *
     * @param state Decoded packet state, or nullptr when decoding failed.
     * @param status Packet acceptance status for the CSV record.
     */
    void write_receive_csv(const received_state_t *state, std::string_view status) {
      static std::ofstream csv;
      static bool initialized = false;
      if (!initialized) {
        initialized = true;
        const auto *configured_path = std::getenv("SUNSHINE_GAZE_CSV");
        const auto path = configured_path ? configured_path : "gaze-receiver.csv";
        if (*path) {
          csv.open(path, std::ios::out | std::ios::app);
          csv.imbue(std::locale::classic());
          if (csv && csv.tellp() == 0) {
            csv << "utc_epoch_seconds,sequence,tracker_timestamp,center_x,center_y,radius_1,radius_2,radius_3,radius_4,confidence,valid,status\n";
          }
          if (!csv) {
            BOOST_LOG(warning) << "Could not open gaze receiver CSV [" << path << ']';
          }
        }
      }
      if (!csv) {
        return;
      }

      const auto received_at = std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
      csv << received_at << ',';
      if (state) {
        csv << state->sequence << ',' << state->tracker_timestamp << ',' << state->center_x << ',' << state->center_y << ','
            << state->radii[0] << ',' << state->radii[1] << ',' << state->radii[2] << ',' << state->radii[3] << ','
            << static_cast<unsigned int>(state->confidence) << ',' << (state->valid ? "true" : "false") << ',' << status << '\n';
      } else {
        csv << ",,,,,,,,,," << status << '\n';
      }
      csv.flush();
    }

    bool is_finite_nonnegative(float value) {
      return std::isfinite(value) && value >= 0.0f;
    }

    bool is_newer_sequence(std::uint64_t incoming, std::uint64_t current) {
      const auto distance = incoming - current;
      return distance != 0 && distance < (std::uint64_t {1} << 63);
    }

    void accept_packet(const std::uint8_t *data, std::size_t size) {
      flatbuffers::Verifier verifier(data, size);
      if (!Sunshine::Foveation::VerifyGazePacketBuffer(verifier)) {
        write_receive_csv(nullptr, "malformed");
        BOOST_LOG(debug) << "Discarded malformed gaze FlatBuffer packet";
        return;
      }

      const auto *packet = Sunshine::Foveation::GetGazePacket(data);
      if (packet->protocol_version() != 1) {
        write_receive_csv(nullptr, "unsupported_version");
        BOOST_LOG(debug) << "Discarded unsupported gaze protocol version [" << packet->protocol_version() << ']';
        return;
      }

      received_state_t next;
      next.sequence = packet->sequence();
      next.tracker_timestamp = packet->tracker_timestamp();
      next.center_x = packet->center_x();
      next.center_y = packet->center_y();
      next.radii = {packet->radius_level_1(), packet->radius_level_2(), packet->radius_level_3(), packet->radius_level_4()};
      next.confidence = packet->confidence();
      next.valid = packet->valid() && std::isfinite(next.tracker_timestamp) && next.tracker_timestamp >= 0.0 && std::isfinite(next.center_x) && std::isfinite(next.center_y) && next.center_x >= 0.0f && next.center_x <= 1.0f && next.center_y >= 0.0f && next.center_y <= 1.0f && next.confidence >= 1 && next.confidence <= 3 && std::all_of(next.radii.begin(), next.radii.end(), is_finite_nonnegative) && next.radii[0] <= next.radii[1] && next.radii[1] <= next.radii[2] && next.radii[2] <= next.radii[3] && next.radii[3] <= 2.0f;
      next.received_at = std::chrono::steady_clock::now();

      bool reordered = false;
      {
        std::lock_guard lock(state_mutex);
        const auto sender_expired = has_sequence && next.received_at - latest_state.received_at > GAZE_TIMEOUT;
        reordered = has_sequence && !sender_expired && !is_newer_sequence(next.sequence, latest_state.sequence);
        if (!reordered) {
          latest_state = next;
          has_sequence = true;
        }
      }
      if (reordered) {
        write_receive_csv(&next, "reordered");
        BOOST_LOG(debug) << "Discarded reordered gaze packet sequence [" << next.sequence << ']';
        return;
      }
      write_receive_csv(&next, next.valid ? "accepted" : "invalid");
      BOOST_LOG(debug) << "Received gaze sequence [" << next.sequence << "] timestamp [" << next.tracker_timestamp
                       << "] center [" << next.center_x << ',' << next.center_y << "] radii ["
                       << next.radii[0] << ',' << next.radii[1] << ',' << next.radii[2] << ',' << next.radii[3]
                       << "] confidence [" << static_cast<int>(next.confidence) << "] valid [" << next.valid << ']';
    }

    class receiver_t {
    public:
      receiver_t():
          socket(io),
          port(gaze_port()) {
        boost::system::error_code ec;
        socket.open(udp::v4(), ec);
        if (!ec) {
          socket.bind(udp::endpoint(udp::v4(), port), ec);
        }
        if (ec) {
          BOOST_LOG(error) << "Could not start gaze UDP receiver on port [" << port << "]: " << ec.message();
          return;
        }

        receive();
        worker = std::thread([this] {
          io.run();
        });
        BOOST_LOG(info) << "Gaze UDP receiver listening on port [" << port << ']';
      }

      ~receiver_t() {
        io.stop();
        if (worker.joinable()) {
          worker.join();
        }
      }

    private:
      void receive() {
        socket.async_receive_from(boost::asio::buffer(buffer), sender, [this](const boost::system::error_code &error, std::size_t size) {
          if (!error) {
            accept_packet(buffer.data(), size);
          }
          if (socket.is_open()) {
            receive();
          }
        });
      }

      boost::asio::io_context io;
      udp::socket socket;
      std::uint16_t port;
      udp::endpoint sender;
      std::array<std::uint8_t, 512> buffer {};
      std::thread worker;
    };

    receiver_t &receiver() {
      static receiver_t instance;
      return instance;
    }

    std::optional<state_t> current_state() {
      const auto now = std::chrono::steady_clock::now();
      std::lock_guard lock(state_mutex);
      if (!latest_state.valid || now - latest_state.received_at > GAZE_TIMEOUT) {
        return std::nullopt;
      }
      return static_cast<const state_t &>(latest_state);
    }
  }  // namespace

  void start_receiver() {
    static_cast<void>(receiver());
  }

  std::optional<std::array<float, 3>> blur_color() {
    static const auto color = []() -> std::optional<std::array<float, 3>> {
      auto *value = std::getenv("SUNSHINE_GAZE_BLUR_COLOR");
      if (!value || !*value) {
        return std::nullopt;
      }
      std::string_view hex(value);
      if (hex.starts_with('#')) {
        hex.remove_prefix(1);
      }
      if (hex.size() != 6) {
        BOOST_LOG(warning) << "Invalid SUNSHINE_GAZE_BLUR_COLOR value [" << value << "]; expected RRGGBB";
        return std::nullopt;
      }
      std::array<float, 3> rgb {};
      for (std::size_t i = 0; i < rgb.size(); ++i) {
        unsigned int component {};
        const auto result = std::from_chars(hex.data() + i * 2, hex.data() + i * 2 + 2, component, 16);
        if (result.ec != std::errc {} || result.ptr != hex.data() + i * 2 + 2) {
          BOOST_LOG(warning) << "Invalid SUNSHINE_GAZE_BLUR_COLOR value [" << value << "]; expected RRGGBB";
          return std::nullopt;
        }
        rgb[i] = static_cast<float>(component) / 255.0f;
      }
      return rgb;
    }();
    return color;
  }

  std::optional<state_t> latest_state() {
    return current_state();
  }
}  // namespace foveation

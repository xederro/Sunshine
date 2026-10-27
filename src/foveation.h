/**
 * @file src/foveation.h
 * @brief Out-of-band gaze reception for GPU-side foveation.
 */
#pragma once

// standard includes
#include <array>
#include <cstdint>
#include <optional>

namespace foveation {
  inline constexpr std::uint16_t DEFAULT_GAZE_PORT = 48001;  ///< Default UDP port for gaze packets, outside Sunshine's GameStream port range.

  struct state_t {
    std::uint64_t sequence {};
    double tracker_timestamp {-1.0};
    float center_x {};
    float center_y {};
    std::array<float, 4> radii {};
    std::uint8_t confidence {};
  };

  /**
   * @brief Start the process-wide UDP gaze receiver.
   *
   * The receiver binds UDP port DEFAULT_GAZE_PORT by default, or the port configured with
   * SUNSHINE_GAZE_PORT, and keeps the newest verified FlatBuffers packet.
   * Repeated calls are safe.
   */
  void start_receiver();

  /**
   * @brief Get the optional RGB tint applied to gaze-blurred pixels.
   *
   * SUNSHINE_GAZE_BLUR_COLOR accepts six hexadecimal digits, optionally prefixed with #.
   * @return Normalized RGB components, or std::nullopt when tinting is disabled or invalid.
   */
  std::optional<std::array<float, 3>> blur_color();

  /**
   * @brief Get the newest gaze sample while it is valid and no older than 250 ms.
   *
   * @return A fresh gaze sample, or std::nullopt when blur must be disabled.
   */
  std::optional<state_t> latest_state();
}  // namespace foveation

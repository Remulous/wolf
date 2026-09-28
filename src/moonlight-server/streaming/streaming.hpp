#pragma once
#include <algorithm>
#include <boost/asio.hpp>
#include <chrono>
#include <core/gstreamer.hpp>
#include <core/virtual-display.hpp>
#include <events/events.hpp>
#include <fmt/format.h>
#include <gst-plugin/gstrtpmoonlightpay_audio.hpp>
#include <gst-plugin/gstrtpmoonlightpay_video.hpp>
#include <gst-plugin/video.hpp>
#include <gst-video-context.hpp>
#include <gst/gst.h>
#include <gstreamer-1.0/gst/app/gstappsrc.h>
#include <immer/box.hpp>
#include <limits>
#include <memory>
#include <moonlight/fec.hpp>
#include <mutex>

namespace streaming {

using namespace wolf::core;
using boost::asio::ip::udp;

class AdaptiveFecController {
public:
  using clock = std::chrono::steady_clock;

  AdaptiveFecController(bool enabled, int baseline_percentage, clock::time_point started_at = clock::now())
      : enabled_(enabled), baseline_(std::clamp(baseline_percentage, 0, 100)),
        floor_(enabled ? std::max(5, baseline_ / 2) : baseline_), current_(baseline_), last_decrease_(started_at) {}

  void report(const events::VideoFecStatusEvent &status, clock::time_point now = clock::now()) {
    if (!enabled_) {
      return;
    }
    std::scoped_lock lock(mutex_);
    const auto total_shards = static_cast<unsigned int>(status.total_data_packets) + status.total_parity_packets;
    if (total_shards == 0) {
      return;
    }

    const auto loss_percentage = (static_cast<unsigned int>(status.missing_packets) * 100 + total_shards - 1) /
                                 total_shards;
    // Keep roughly twice the observed loss rate plus a small safety margin.
    // Increases are immediate; sustained clean reports reduce overhead slowly.
    const auto target = std::clamp(static_cast<int>(loss_percentage * 2 + 5), floor_, ceiling_);
    if (target > current_) {
      current_ = target;
      last_decrease_ = now;
    } else if (target < current_ && now - last_decrease_ >= decrease_interval_) {
      current_ = std::max(target, current_ - 5);
      last_decrease_ = now;
    }
  }

  int desired_percentage() {
    if (!enabled_) {
      return baseline_;
    }

    std::scoped_lock lock(mutex_);
    return current_;
  }

private:
  bool enabled_;
  int baseline_;
  int floor_;
  int current_;
  static constexpr int ceiling_ = 50;
  static constexpr auto decrease_interval_ = std::chrono::seconds(5);
  clock::time_point last_decrease_;
  std::mutex mutex_;
};

class AdaptiveBitrateController {
public:
  using clock = std::chrono::steady_clock;

  explicit AdaptiveBitrateController(long baseline_kbps, clock::time_point started_at = clock::now())
      : baseline_kbps_(std::max(1L, baseline_kbps)),
        floor_kbps_(std::min(baseline_kbps_, std::max(1000L, baseline_kbps_ * 40 / 100))),
        current_kbps_(baseline_kbps_), last_congestion_(started_at), last_increase_(started_at) {}

  void report(const events::VideoFecStatusEvent &status, clock::time_point now = clock::now()) {
    const auto total_packets = static_cast<unsigned int>(status.total_data_packets) + status.total_parity_packets;
    if (total_packets == 0 || status.missing_packets == 0) {
      return;
    }
    const auto loss_percentage = (static_cast<unsigned int>(status.missing_packets) * 100 + total_packets - 1) /
                                 total_packets;
    report_loss(loss_percentage, now);
  }

  void report(const events::VideoLossStatsEvent &status,
              std::uint32_t last_sent_frame,
              clock::time_point now = clock::now()) {
    const auto last_good_frame = static_cast<std::uint32_t>(status.last_good_frame);
    const auto lag = last_sent_frame - last_good_frame;
    // Legacy reports contain no packet counts. A sustained frame lag is the
    // only congestion signal available, so leave enough room for normal decode
    // and reporting latency before reducing quality.
    if (last_sent_frame != 0 && lag < (std::numeric_limits<std::uint32_t>::max() / 2) && lag > 6) {
      report_loss(lag > 15 ? 10 : 3, now);
    }
  }

  long desired_bitrate_kbps(clock::time_point now = clock::now()) {
    std::scoped_lock lock(mutex_);
    if (current_kbps_ < baseline_kbps_ && now - last_congestion_ >= recovery_delay_ &&
        now - last_increase_ >= increase_interval_) {
      current_kbps_ = std::min(baseline_kbps_, current_kbps_ + std::max(1L, baseline_kbps_ / 20));
      last_increase_ = now;
    }
    return current_kbps_;
  }

private:
  void report_loss(unsigned int loss_percentage, clock::time_point now) {
    std::scoped_lock lock(mutex_);
    last_congestion_ = now;
    if (has_reduced_ && now - last_reduction_ < reduction_interval_) {
      return;
    }

    const long multiplier = loss_percentage >= 10 ? 70 : (loss_percentage >= 3 ? 80 : 90);
    current_kbps_ = std::max(floor_kbps_, current_kbps_ * multiplier / 100);
    last_reduction_ = now;
    last_increase_ = now;
    has_reduced_ = true;
  }

  long baseline_kbps_;
  long floor_kbps_;
  long current_kbps_;
  bool has_reduced_ = false;
  static constexpr auto reduction_interval_ = std::chrono::seconds(1);
  static constexpr auto recovery_delay_ = std::chrono::seconds(5);
  static constexpr auto increase_interval_ = std::chrono::seconds(2);
  clock::time_point last_congestion_;
  clock::time_point last_reduction_{};
  clock::time_point last_increase_;
  std::mutex mutex_;
};

struct WaylandDisplayReady {
  /**
   * The name of the wayland socket that our custom compositor is listening on
   */
  std::string wayland_socket_name;
  /**
   * The wayland plugin element,
   * we need a reference so that we can send events directly to it (mouse, keyboard, ...)
   */
  gstreamer::gst_element_ptr wayland_plugin;
};

void start_video_producer(const std::string &session_id,
                          const std::string &buffer_format,
                          const std::string &render_node,
                          const wolf::core::virtual_display::DisplayMode &display_mode,
                          std::shared_ptr<immer::atom<gst_video_context::gst_context_ptr>> video_context,
                          std::shared_ptr<boost::promise<WaylandDisplayReady>> on_ready,
                          std::shared_ptr<events::EventBusType> event_bus);

void start_audio_producer(const std::string &session_id,
                          const std::shared_ptr<events::EventBusType> &event_bus,
                          int channel_count,
                          const std::string &sink_name,
                          const std::string &server_name);

void start_streaming_video(immer::box<events::VideoSession> video_session,
                           const std::shared_ptr<events::EventBusType> &event_bus,
                           std::string client_ip,
                           unsigned short client_port,
                           std::shared_ptr<immer::atom<gst_video_context::gst_context_ptr>> video_context,
                           std::shared_ptr<udp::socket> video_socket);

void start_streaming_audio(immer::box<events::AudioSession> audio_session,
                           const std::shared_ptr<events::EventBusType> &event_bus,
                           std::string client_ip,
                           unsigned short client_port,
                           std::shared_ptr<udp::socket> audio_socket,
                           const std::string &sink_name,
                           const std::string &server_name);

static bool run_pipeline(
    const std::string &pipeline_desc,
    const std::function<immer::array<immer::box<events::EventBusHandlers>>(gstreamer::gst_element_ptr /* pipeline */)>
        &on_pipeline_ready) {
  GError *error = nullptr;
  gstreamer::gst_element_ptr pipeline(gst_parse_launch(pipeline_desc.c_str(), &error), [](const auto &pipeline) {
    logs::log(logs::trace, "~pipeline");
    gst_object_unref(pipeline);
  });

  if (!pipeline) {
    logs::log(logs::error, "[GSTREAMER] Pipeline parse error: {}", error->message);
    g_error_free(error);
    return false;
  } else if (error) { // Please note that you might get a return value that is not NULL even though the error is set. In
                      // this case there was a recoverable parsing error and you can try to play the pipeline.
    logs::log(logs::warning, "[GSTREAMER] Pipeline parse error (recovered): {}", error->message);
    g_error_free(error);
  }

  gstreamer::gst_main_context_ptr context = {g_main_context_new(), ::g_main_context_unref};
  g_main_context_push_thread_default(context.get());
  gstreamer::gst_main_loop_ptr loop(g_main_loop_new(context.get(), FALSE), ::g_main_loop_unref);

  /* Let the calling thread set extra things */
  auto handlers = on_pipeline_ready(pipeline);

  /*
   * adds a watch for new message on our pipeline's message bus to
   * the default GLib main context, which is the main context that our
   * GLib main loop is attached to below
   */
  auto bus = gst_pipeline_get_bus(GST_PIPELINE(pipeline.get()));
  gst_bus_add_signal_watch(bus);
  g_signal_connect(bus, "message::error", G_CALLBACK(gstreamer::pipeline_error_handler), loop.get());
  g_signal_connect(bus, "message::eos", G_CALLBACK(gstreamer::pipeline_eos_handler), loop.get());
  gst_object_unref(bus);

  /* Set the pipeline to "playing" state*/
  gst_element_set_state(pipeline.get(), GST_STATE_PLAYING);
  GST_DEBUG_BIN_TO_DOT_FILE_WITH_TS(reinterpret_cast<GstBin *>(pipeline.get()),
                                    GST_DEBUG_GRAPH_SHOW_ALL,
                                    "pipeline-start");

  /* The main loop will be run until someone calls g_main_loop_quit() */
  g_main_loop_run(loop.get());

  /* Out of the main loop, clean up nicely */
  gst_element_set_state(pipeline.get(), GST_STATE_PAUSED);
  gst_element_set_state(pipeline.get(), GST_STATE_READY);
  gst_element_set_state(pipeline.get(), GST_STATE_NULL);

  return true;
}

/**
 * @return the Gstreamer version we are linked to
 */
inline std::string get_gst_version() {
  guint major, minor, micro, nano;
  gst_version(&major, &minor, &micro, &nano);
  return fmt::format("{}.{}.{}-{}", major, minor, micro, nano);
}

/**
 * GStreamer needs to be initialised once per run
 * Call this method in your main.
 */
inline void init() {
  /* It is also possible to call the init function with two NULL arguments,
   * in which case no command line options will be parsed by GStreamer.
   */
  gst_init(nullptr, nullptr);
  logs::log(logs::info, "Gstreamer version: {}", get_gst_version());

  gst_element_register(nullptr, "rtpmoonlightpay_video", GST_RANK_PRIMARY, gst_TYPE_rtp_moonlight_pay_video);
  gst_element_register(nullptr, "rtpmoonlightpay_audio", GST_RANK_PRIMARY, gst_TYPE_rtp_moonlight_pay_audio);

  moonlight::fec::init();
}

} // namespace streaming

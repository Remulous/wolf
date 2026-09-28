#include "platforms/hw.hpp"

#include <atomic>
#include <chrono>
#include <control/control.hpp>
#include <core/batched_send.hpp>
#include <gst-video-context.hpp>
#include <gstreamer-1.0/gst/app/gstappsink.h>
#include <gstreamer-1.0/gst/app/gstappsrc.h>
#include <helpers/utils.hpp>
#include <immer/array.hpp>
#include <immer/box.hpp>
#include <memory>
#include <streaming/streaming.hpp>
#include <thread>

namespace streaming {

using namespace wolf::core::gstreamer;
using namespace wolf::core;

struct GstBusData {
  std::shared_ptr<boost::promise<WaylandDisplayReady>> on_ready;
  gst_element_ptr wayland_plugin;
};

gboolean structure_each(GQuark field_id, const GValue *value, gpointer user_data) {
  auto field_str = std::string(g_quark_to_string(field_id));
  if (!G_VALUE_HOLDS_STRING(value)) {
    logs::log(logs::warning, "Wayland source message: {} = {}", field_str, "not a string");
    return FALSE;
  }
  auto value_str = g_value_get_string(value);
  logs::log(logs::debug, "Wayland source message: {} = {}", field_str, value_str);

  if (field_str == "WAYLAND_DISPLAY") {
    logs::log(logs::info, "Wayland display ready, listening on: {}", value_str);
    auto bus_data = static_cast<GstBusData *>(user_data);
    bus_data->on_ready->set_value(
        WaylandDisplayReady{.wayland_socket_name = value_str, .wayland_plugin = bus_data->wayland_plugin});
  }

  return TRUE;
}

static void application_message_handler(GstBus *bus, GstMessage *msg, gpointer data) {
  auto structure = gst_message_get_structure(msg);
  if (gst_structure_has_name(structure, "wayland.src")) {
    gst_structure_foreach(structure, structure_each, data);
  }
}

struct NeedContextData {
  const std::string device_path;
  std::shared_ptr<immer::atom<gst_video_context::gst_context_ptr>> gst_context;
};

static void need_context_handler(GstBus *bus, GstMessage *msg, gpointer data) {
  auto ctx_data = static_cast<NeedContextData *>(data);
  auto cached_context = ctx_data->gst_context->load();
  if (cached_context.get() && gst_video_context::set_context(cached_context.get(), msg)) {
    logs::log(logs::debug, "Context already set, passing it to the pipeline.");
    return;
  }
  if (auto video_context = gst_video_context::need_context_for_device(ctx_data->device_path, msg)) {
    ctx_data->gst_context->store(video_context);
  }
}

static GstBusSyncReply bus_sync_handler(GstBus *bus, GstMessage *msg, gpointer data) {
  if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_NEED_CONTEXT) {
    need_context_handler(bus, msg, data);
  }
  return GST_BUS_PASS;
}

std::pair<std::string, std::string> get_color_params(immer::box<events::VideoSession> video_session) {
  std::string color_range = (video_session->color_range == events::ColorRange::JPEG) ? "jpeg" : "mpeg2";
  std::string color_space;
  switch (video_session->color_space) {
  case events::ColorSpace::BT601:
    color_space = "bt601";
    break;
  case events::ColorSpace::BT709:
    color_space = "bt709";
    break;
  case events::ColorSpace::BT2020:
    color_space = "bt2020";
    break;
  }
  return std::make_pair(color_range, color_space);
}

void start_video_producer(const std::string &session_id,
                          const std::string &buffer_format,
                          const std::string &render_node,
                          const wolf::core::virtual_display::DisplayMode &display_mode,
                          std::shared_ptr<immer::atom<gst_video_context::gst_context_ptr>> video_context,
                          std::shared_ptr<boost::promise<WaylandDisplayReady>> on_ready,
                          std::shared_ptr<events::EventBusType> event_bus) {
  auto pipeline = fmt::format("waylanddisplaysrc name=wolf_wayland_source render_node={render_node} ! "
                              "{buffer_format}, width={width}, height={height}, framerate={fps}/1 ! \n"    //
                              "interpipesink sync=true async=false name={session_id}_video max-buffers=1", //
                              fmt::arg("buffer_format", buffer_format),
                              fmt::arg("render_node", render_node),
                              fmt::arg("session_id", session_id),
                              fmt::arg("width", display_mode.width),
                              fmt::arg("height", display_mode.height),
                              fmt::arg("fps", display_mode.refreshRate));
  logs::log(logs::debug, "[GSTREAMER] Starting video producer: {}", pipeline);
  auto bus_data_ptr =
      std::make_shared<GstBusData>(GstBusData{.on_ready = std::move(on_ready), .wayland_plugin = nullptr});
  std::shared_ptr<NeedContextData> ctx_data_ptr =
      std::make_shared<NeedContextData>(NeedContextData{.device_path = render_node, .gst_context = video_context});
  run_pipeline(pipeline, [=](auto pipeline) {
    logs::log(logs::debug, "Setting up waylanddisplaysrc");

    auto wayland_plugin_el = gst_bin_get_by_name(GST_BIN(pipeline.get()), "wolf_wayland_source");
    auto wayland_plugin_ptr = gst_element_ptr(wayland_plugin_el, ::gst_object_unref);
    bus_data_ptr->wayland_plugin.swap(wayland_plugin_ptr);

    auto bus = gst_pipeline_get_bus(GST_PIPELINE(pipeline.get()));
    g_signal_connect(bus, "message::application", G_CALLBACK(application_message_handler), bus_data_ptr.get());
    gst_bus_set_sync_handler(bus, bus_sync_handler, ctx_data_ptr.get(), nullptr);
    gst_object_unref(bus);

    auto stop_handler = event_bus->register_handler<immer::box<events::StopStreamEvent>>(
        [session_id, pipeline](const immer::box<events::StopStreamEvent> &ev) {
          if (std::to_string(ev->session_id) == session_id) {
            logs::log(logs::debug, "[GSTREAMER] Stopping video producer: {}", session_id);
            gst_element_send_event(pipeline.get(), gst_event_new_eos());
          }
        });

    auto stop_lobby_handler = event_bus->register_handler<immer::box<events::StopLobbyEvent>>(
        [session_id, pipeline](const immer::box<events::StopLobbyEvent> &ev) {
          if (ev->lobby_id == session_id) {
            logs::log(logs::debug, "[GSTREAMER] Stopping video producer: {}", session_id);
            gst_element_send_event(pipeline.get(), gst_event_new_eos());
          }
        });

    return immer::array<immer::box<events::EventBusHandlers>>{std::move(stop_handler), std::move(stop_lobby_handler)};
  });
}

void start_audio_producer(const std::string &session_id,
                          const std::shared_ptr<events::EventBusType> &event_bus,
                          int channel_count,
                          const std::string &sink_name,
                          const std::string &server_name) {
  std::string channel_mask;
  switch (channel_count) {
  case 2:
    channel_mask = "0x3";
    break;
  case 6:
    channel_mask = "0x3f";
    break;
  case 8:
    channel_mask = "0xc3f";
    break;
  default:
    channel_mask = "";
  }

  auto pipeline = fmt::format("pulsesrc device=\"{sink_name}\" server=\"{server_name}\" ! "                           //
                              "audio/x-raw, channels={channels}, channel-mask=(bitmask){channel_mask}, rate=48000 ! " //
                              "queue leaky=downstream max-size-buffers=3 ! "                                          //
                              "interpipesink name=\"{session_id}_audio\" sync=true async=false max-buffers=3",
                              fmt::arg("session_id", session_id),
                              fmt::arg("channels", channel_count),
                              fmt::arg("channel_mask", channel_mask),
                              fmt::arg("sink_name", sink_name),
                              fmt::arg("server_name", server_name));
  logs::log(logs::debug, "[GSTREAMER] Starting audio producer: {}", pipeline);

  run_pipeline(pipeline, [=](auto pipeline) {
    auto stop_handler = event_bus->register_handler<immer::box<events::StopStreamEvent>>(
        [session_id, pipeline](const immer::box<events::StopStreamEvent> &ev) {
          if (std::to_string(ev->session_id) == session_id) {
            logs::log(logs::debug, "[GSTREAMER] Stopping audio producer: {}", session_id);
            gst_element_send_event(pipeline.get(), gst_event_new_eos());
          }
        });

    auto stop_lobby_handler = event_bus->register_handler<immer::box<events::StopLobbyEvent>>(
        [session_id, pipeline](const immer::box<events::StopLobbyEvent> &ev) {
          if (ev->lobby_id == session_id) {
            logs::log(logs::debug, "[GSTREAMER] Stopping video producer: {}", session_id);
            gst_element_send_event(pipeline.get(), gst_event_new_eos());
          }
        });

    return immer::array<immer::box<events::EventBusHandlers>>{std::move(stop_handler), std::move(stop_lobby_handler)};
  });
}

namespace custom_sink {

struct PacingConfig {
  bool enabled = false;
  std::uint64_t encoder_bitrate_bps = 0;
  std::uint64_t bitrate_bps = 0;
  std::size_t packet_size = 1;
  /**
   * Maximum number of packets per sendmmsg() syscall.
   * This is also capped to roughly 1 ms of traffic so a batch does not become
   * the same kind of microburst that pacing is intended to prevent.
   */
  std::size_t max_batch_size = 16;
  std::chrono::steady_clock::time_point next_send_time = std::chrono::steady_clock::now();
};

struct UDPSink {
  std::shared_ptr<udp::socket> socket;
  std::shared_ptr<udp::endpoint> client_endpoint;
  PacingConfig pacing;
  std::shared_ptr<AdaptiveFecController> adaptive_fec;
  std::shared_ptr<AdaptiveBitrateController> adaptive_bitrate;
  gst_element_ptr video_payloader;
  gst_element_ptr video_encoder;
  int active_fec_percentage = 0;
  long active_encoder_bitrate_kbps = 0;
  std::shared_ptr<std::atomic<std::uint32_t>> last_sent_frame = std::make_shared<std::atomic<std::uint32_t>>(0);
  wolf::platform::batched_send_info_t send_info;
};

static std::uint64_t pacing_bitrate(std::uint64_t encoder_bitrate_bps, int fec_percentage) {
  return encoder_bitrate_bps * static_cast<std::uint64_t>(100 + std::max(0, fec_percentage)) * 125 / 100 / 100;
}

static void apply_adaptive_fec_for_next_frame(UDPSink *udp_sink) {
  if (!udp_sink->adaptive_fec || !udp_sink->video_payloader) {
    return;
  }
  const auto desired_fec = udp_sink->adaptive_fec->desired_percentage();
  if (desired_fec == udp_sink->active_fec_percentage) {
    return;
  }

  logs::log(logs::debug,
            "[GSTREAMER] Adjusting video FEC from {}% to {}%",
            udp_sink->active_fec_percentage,
            desired_fec);
  g_object_set(udp_sink->video_payloader.get(), "fec_percentage", desired_fec, nullptr);
  udp_sink->active_fec_percentage = desired_fec;
}

static gst_element_ptr find_video_encoder(GstElement *pipeline) {
  GstIterator *iterator = gst_bin_iterate_recurse(GST_BIN(pipeline));
  GValue item = G_VALUE_INIT;
  gst_element_ptr result;

  while (gst_iterator_next(iterator, &item) == GST_ITERATOR_OK) {
    auto *element = GST_ELEMENT(g_value_get_object(&item));
    auto *factory = gst_element_get_factory(element);
    const auto *klass = factory ? gst_element_factory_get_metadata(factory, GST_ELEMENT_METADATA_KLASS) : nullptr;
    if (klass && std::string_view(klass).find("Encoder/Video") != std::string_view::npos) {
      auto *object_class = G_OBJECT_GET_CLASS(element);
      if (g_object_class_find_property(object_class, "bitrate") ||
          g_object_class_find_property(object_class, "target-bitrate")) {
        result = gst_element_ptr(GST_ELEMENT(gst_object_ref(element)), ::gst_object_unref);
        g_value_reset(&item);
        break;
      }
    }
    g_value_reset(&item);
  }

  g_value_unset(&item);
  gst_iterator_free(iterator);
  return result;
}

static bool set_encoder_bitrate(GstElement *encoder, long bitrate_kbps) {
  const auto *factory_name = gst_plugin_feature_get_name(GST_PLUGIN_FEATURE(gst_element_get_factory(encoder)));
  const char *property_name = g_object_class_find_property(G_OBJECT_GET_CLASS(encoder), "bitrate") ? "bitrate"
                                                                                                   : "target-bitrate";
  auto *property = g_object_class_find_property(G_OBJECT_GET_CLASS(encoder), property_name);
  if (!property || !(property->flags & G_PARAM_WRITABLE)) {
    return false;
  }

  // Apple VideoToolbox and OpenH264 expose bits/sec. The encoders shipped in
  // Wolf's current defaults expose kbits/sec.
  std::uint64_t value = static_cast<std::uint64_t>(bitrate_kbps);
  const std::string_view factory = factory_name ? factory_name : "";
  if (factory.starts_with("vtenc_") || factory == "openh264enc") {
    value *= 1000;
  }

  GValue property_value = G_VALUE_INIT;
  g_value_init(&property_value, G_PARAM_SPEC_VALUE_TYPE(property));
  if (G_VALUE_HOLDS_INT(&property_value)) {
    g_value_set_int(&property_value, static_cast<gint>(value));
  } else if (G_VALUE_HOLDS_UINT(&property_value)) {
    g_value_set_uint(&property_value, static_cast<guint>(value));
  } else if (G_VALUE_HOLDS_INT64(&property_value)) {
    g_value_set_int64(&property_value, static_cast<gint64>(value));
  } else if (G_VALUE_HOLDS_UINT64(&property_value)) {
    g_value_set_uint64(&property_value, value);
  } else {
    g_value_unset(&property_value);
    return false;
  }
  g_object_set_property(G_OBJECT(encoder), property_name, &property_value);
  g_value_unset(&property_value);
  return true;
}

static void apply_adaptive_bitrate_for_next_frame(UDPSink *udp_sink) {
  if (!udp_sink->adaptive_bitrate || !udp_sink->video_encoder) {
    return;
  }
  const auto desired_bitrate = udp_sink->adaptive_bitrate->desired_bitrate_kbps();
  if (desired_bitrate == udp_sink->active_encoder_bitrate_kbps) {
    return;
  }
  if (!set_encoder_bitrate(udp_sink->video_encoder.get(), desired_bitrate)) {
    logs::log(logs::warning, "[GSTREAMER] Encoder does not support runtime bitrate adjustment");
    udp_sink->video_encoder.reset();
    return;
  }

  logs::log(logs::info,
            "[GSTREAMER] Adjusting video bitrate from {} to {} Kbps",
            udp_sink->active_encoder_bitrate_kbps,
            desired_bitrate);
  udp_sink->active_encoder_bitrate_kbps = desired_bitrate;
  udp_sink->pacing.encoder_bitrate_bps = static_cast<std::uint64_t>(desired_bitrate) * 1000;
}

static void ensure_socket_open(UDPSink *udp_sink, bool is_video) {
  if (!udp_sink->socket->is_open()) {
    logs::log(logs::warning, "UDP Socket is not open");
    udp_sink->socket->open(udp::v4());
    wolf::platform::configure_socket_for_streaming(*udp_sink->socket, is_video);
    wolf::platform::enable_socket_qos(udp_sink->socket->native_handle(), is_video);
  }
}

static GstFlowReturn send_buffer_batched(GstBufferList *buffer_list, UDPSink *udp_sink) {
  guint num_buffers = gst_buffer_list_length(buffer_list);
  if (num_buffers == 0) {
    return GST_FLOW_OK;
  }

  ensure_socket_open(udp_sink, true);

  std::vector<std::pair<GstBuffer *, GstMapInfo>> mapped_buffers;
  udp_sink->send_info.payload_buffers.resize(num_buffers);
  mapped_buffers.reserve(num_buffers);

  for (guint i = 0; i < num_buffers; i++) {
    GstBuffer *buffer = gst_buffer_list_get(buffer_list, i);

    GstMapInfo map;
    if (!gst_buffer_map(buffer, &map, GST_MAP_READ)) {
      logs::log(logs::error, "Failed to map buffer {} in batch", i);
      for (auto &[mapped_buffer, m] : mapped_buffers) {
        gst_buffer_unmap(mapped_buffer, &m);
      }
      return GST_FLOW_ERROR;
    }
    mapped_buffers.emplace_back(buffer, map);
    if (i == 0 && map.size >= sizeof(gst_moonlight_video::VideoRTPHeaders)) {
      const auto *headers = reinterpret_cast<const gst_moonlight_video::VideoRTPHeaders *>(map.data);
      udp_sink->last_sent_frame->store(boost::endian::little_to_native(headers->packet.frameIndex),
                                       std::memory_order_relaxed);
    }
    udp_sink->send_info.payload_buffers[i] =
        wolf::platform::buffer_descriptor_t(reinterpret_cast<const char *>(map.data), map.size);
  }

  udp_sink->send_info.native_socket = udp_sink->socket->native_handle();
  udp_sink->send_info.target_address = udp_sink->client_endpoint->address();
  udp_sink->send_info.target_port = udp_sink->client_endpoint->port();

  bool success = true;

  if (!udp_sink->pacing.enabled) {
    udp_sink->send_info.block_offset = 0;
    udp_sink->send_info.block_count = num_buffers;
    success = wolf::platform::send_batch(udp_sink->send_info);
  } else {
    auto &pacing = udp_sink->pacing;
    pacing.bitrate_bps = pacing_bitrate(pacing.encoder_bitrate_bps, udp_sink->active_fec_percentage);
    const auto packets_per_ms = std::max<std::uint64_t>(1, pacing.bitrate_bps / 1000 / (pacing.packet_size * 8));
    pacing.max_batch_size = std::min<std::size_t>({16, 65536 / pacing.packet_size, packets_per_ms});
    auto next_send_time = std::max(pacing.next_send_time, std::chrono::steady_clock::now());
    std::size_t packets_sent = 0;

    while (packets_sent < num_buffers) {
      if (auto now = std::chrono::steady_clock::now(); now < next_send_time) {
        std::this_thread::sleep_until(next_send_time);
      }

      std::size_t batch = std::min(pacing.max_batch_size, num_buffers - packets_sent);
      std::size_t batch_bytes = 0;
      for (std::size_t i = 0; i < batch; ++i) {
        batch_bytes += udp_sink->send_info.payload_buffers[packets_sent + i].size;
      }

      udp_sink->send_info.block_offset = packets_sent;
      udp_sink->send_info.block_count = batch;
      if (!wolf::platform::send_batch(udp_sink->send_info)) {
        success = false;
        break;
      }

      packets_sent += batch;
      auto transmission_time = std::chrono::duration<double>(static_cast<double>(batch_bytes) * 8 / pacing.bitrate_bps);
      next_send_time += std::chrono::duration_cast<std::chrono::steady_clock::duration>(transmission_time);
    }

    pacing.next_send_time = next_send_time;
  }

  for (auto &[buffer, m] : mapped_buffers) {
    gst_buffer_unmap(buffer, &m);
  }

  if (!success) {
    logs::log(logs::warning, "Failed to send batch of {} packets", num_buffers);
    return GST_FLOW_ERROR;
  }

  // The current list was produced with active_fec_percentage. Change the
  // upstream payloader only after sending it so the next frame and its pacing
  // budget switch percentages together.
  apply_adaptive_fec_for_next_frame(udp_sink);
  apply_adaptive_bitrate_for_next_frame(udp_sink);

  return GST_FLOW_OK;
}

static GstFlowReturn send_buffer_single(GstBuffer *buffer, UDPSink *udp_sink) {
  GstMapInfo map;
  if (gst_buffer_map(buffer, &map, GST_MAP_READ)) {
    ensure_socket_open(udp_sink, false);

    wolf::platform::batched_send_info_t send_info;
    send_info.payload_buffers.emplace_back(reinterpret_cast<const char *>(map.data), map.size);
    send_info.block_offset = 0;
    send_info.block_count = 1;
    send_info.native_socket = udp_sink->socket->native_handle();
    send_info.target_address = udp_sink->client_endpoint->address();
    send_info.target_port = udp_sink->client_endpoint->port();

    bool success = wolf::platform::send_batch(send_info);
    gst_buffer_unmap(buffer, &map);

    if (!success) {
      logs::log(logs::error, "Error sending UDP packet");
      return GST_FLOW_ERROR;
    }
    return GST_FLOW_OK;
  } else {
    logs::log(logs::error, "Failed to map buffer");
    return GST_FLOW_ERROR;
  }
}

static GstFlowReturn on_new_sample(GstAppSink *appsink, gpointer user_data) {
  std::shared_ptr<GstSample> sample(gst_app_sink_pull_sample(appsink), gst_sample_unref);
  if (!sample) {
    logs::log(logs::warning, "Custom sink: failed to create sample");
    return GST_FLOW_ERROR;
  }

  UDPSink *udp_sink = static_cast<UDPSink *>(user_data);

  if (GstBufferList *buffer_list = gst_sample_get_buffer_list(sample.get())) {
    return send_buffer_batched(buffer_list, udp_sink);
  } else if (GstBuffer *buffer = gst_sample_get_buffer(sample.get())) {
    return send_buffer_single(buffer, udp_sink);
  } else {
    logs::log(logs::warning, "Custom sink: failed to get buffer");
    return GST_FLOW_ERROR;
  }
}

static void configure_appsink(GstElement *appsink, UDPSink *udp_sink) {
  g_object_set(appsink, "emit-signals", FALSE, NULL);
  g_object_set(appsink, "buffer-list", TRUE, NULL);

  GstAppSinkCallbacks callbacks = {nullptr};
  callbacks.new_sample = on_new_sample;
  gst_app_sink_set_callbacks(GST_APP_SINK(appsink), &callbacks, udp_sink, nullptr);
}
} // namespace custom_sink

static void force_idr(GstElement *pipeline) {
  wolf::core::gstreamer::send_message(pipeline,
                                      gst_structure_new("GstForceKeyUnit", "all-headers", G_TYPE_BOOLEAN, TRUE, NULL));
}

/**
 * Start VIDEO pipeline
 */
void start_streaming_video(immer::box<events::VideoSession> video_session,
                           const std::shared_ptr<events::EventBusType> &event_bus,
                           std::string client_ip,
                           unsigned short client_port,
                           std::shared_ptr<immer::atom<gst_video_context::gst_context_ptr>> video_context,
                           std::shared_ptr<udp::socket> video_socket) {
  auto [color_range, color_space] = get_color_params(video_session);

  auto pipeline = fmt::format(
      fmt::runtime(video_session->gst_pipeline),
      fmt::arg("session_id", video_session->session_id),
      fmt::arg("width", video_session->display_mode.width),
      fmt::arg("height", video_session->display_mode.height),
      fmt::arg("fps", video_session->display_mode.refreshRate),
      fmt::arg("bitrate", video_session->bitrate_kbps),
      fmt::arg("client_port", client_port),
      fmt::arg("client_ip", client_ip),
      fmt::arg("payload_size", video_session->packet_size),
      fmt::arg("fec_percentage", video_session->fec_percentage),
      fmt::arg("min_required_fec_packets", video_session->min_required_fec_packets),
      fmt::arg("slices_per_frame", video_session->slices_per_frame),
      fmt::arg("vbv_buffer_size", video_session->bitrate_kbps / video_session->display_mode.refreshRate),
      fmt::arg("color_space", color_space),
      fmt::arg("color_range", color_range),
      fmt::arg("host_port", video_session->port));
  logs::log(logs::debug, "Starting video pipeline: \n{}", pipeline);

  bool enable_pacing = utils::get_env("WOLF_ENABLE_VIDEO_PACING", "TRUE") == std::string("TRUE");
  // The encoder bitrate excludes FEC packets. Pace the actual wire traffic with
  // 25% headroom so a normally-sized frame is delivered before the next frame
  // arrives without draining it at the old hard-coded 800 Mbps burst rate.
  auto encoder_bitrate_bps = static_cast<std::uint64_t>(std::max(1L, video_session->bitrate_kbps)) * 1000;
  auto pacing_bitrate_bps = custom_sink::pacing_bitrate(encoder_bitrate_bps, video_session->fec_percentage);
  auto packet_size = static_cast<std::size_t>(std::max(1, video_session->packet_size));
  auto packets_per_ms = std::max<std::uint64_t>(1, pacing_bitrate_bps / 1000 / (packet_size * 8));
  std::shared_ptr<custom_sink::UDPSink> udp_sink = std::make_shared<custom_sink::UDPSink>(custom_sink::UDPSink{
      .socket = video_socket,
      .client_endpoint = std::make_shared<udp::endpoint>(boost::asio::ip::make_address(client_ip), client_port),
      .pacing =
          {
              .enabled = enable_pacing,
              .encoder_bitrate_bps = encoder_bitrate_bps,
              .bitrate_bps = pacing_bitrate_bps,
              .packet_size = packet_size,
              .max_batch_size = std::min<std::size_t>({16, 65536 / packet_size, packets_per_ms}),
          },
      .adaptive_fec =
          std::make_shared<AdaptiveFecController>(video_session->adaptive_fec, video_session->fec_percentage),
      .adaptive_bitrate = std::make_shared<AdaptiveBitrateController>(video_session->bitrate_kbps),
      .active_fec_percentage = video_session->fec_percentage,
      .active_encoder_bitrate_kbps = video_session->bitrate_kbps});
  std::shared_ptr<NeedContextData> ctx_data_ptr = std::make_shared<NeedContextData>(
      NeedContextData{.device_path = video_session->render_node, .gst_context = video_context});
  run_pipeline(pipeline, [video_session, event_bus, udp_sink, ctx_data_ptr](auto pipeline) {
    if (auto app_sink_el = gst_bin_get_by_name(GST_BIN(pipeline.get()), "wolf_udp_sink")) {
      logs::log(logs::debug, "Setting up wolf_udp_sink");
      g_assert(GST_IS_APP_SINK(app_sink_el));
      configure_appsink(app_sink_el, udp_sink.get());
      gst_object_unref(app_sink_el);
    }
    if (auto payloader = gst_bin_get_by_name(GST_BIN(pipeline.get()), "moonlight_pay")) {
      udp_sink->video_payloader = gst_element_ptr(payloader, ::gst_object_unref);
    } else if (video_session->adaptive_fec) {
      logs::log(logs::warning, "[GSTREAMER] Adaptive FEC disabled: moonlight_pay element not found");
    }
    udp_sink->video_encoder = custom_sink::find_video_encoder(pipeline.get());
    if (!udp_sink->video_encoder) {
      logs::log(logs::warning, "[GSTREAMER] Adaptive bitrate disabled: video encoder not found");
    }

    auto bus = gst_pipeline_get_bus(GST_PIPELINE(pipeline.get()));
    gst_bus_set_sync_handler(bus, bus_sync_handler, ctx_data_ptr.get(), nullptr);
    gst_object_unref(bus);

    /*
     * The force IDR event will be triggered by the control stream.
     * We have to pass this back into the gstreamer pipeline
     * in order to force the encoder to produce a new IDR packet
     */
    auto idr_handler = event_bus->register_handler<immer::box<events::IDRRequestEvent>>(
        [sess_id = video_session->session_id, pipeline](const immer::box<events::IDRRequestEvent> &ctrl_ev) {
          if (ctrl_ev->session_id == sess_id) {
            logs::log(logs::debug, "[GSTREAMER] Forcing IDR");
            // Force IDR event, see: https://github.com/centricular/gstwebrtc-demos/issues/186
            // https://gstreamer.freedesktop.org/documentation/additional/design/keyframe-force.html?gi-language=c
            force_idr(pipeline.get());
          }
        });

    auto fec_status_handler = event_bus->register_handler<immer::box<events::VideoFecStatusEvent>>(
        [sess_id = video_session->session_id,
         fec_controller = udp_sink->adaptive_fec,
         bitrate_controller = udp_sink->adaptive_bitrate](const immer::box<events::VideoFecStatusEvent> &status) {
          if (status->session_id == sess_id) {
            fec_controller->report(*status);
            bitrate_controller->report(*status);
          }
        });

    auto loss_stats_handler = event_bus->register_handler<immer::box<events::VideoLossStatsEvent>>(
        [sess_id = video_session->session_id,
         controller = udp_sink->adaptive_bitrate,
         last_sent_frame = udp_sink->last_sent_frame](const immer::box<events::VideoLossStatsEvent> &status) {
          if (status->session_id == sess_id) {
            controller->report(*status, last_sent_frame->load(std::memory_order_relaxed));
          }
        });

    auto invalidate_handler = event_bus->register_handler<immer::box<events::ReferenceFrameInvalidationEvent>>(
        [sess_id = video_session->session_id,
         pipeline](const immer::box<events::ReferenceFrameInvalidationEvent> &request) {
          if (request->session_id == sess_id) {
            // GStreamer has no codec-agnostic reference-frame invalidation API.
            // Preserve correct recovery behavior by falling back to an IDR.
            logs::log(logs::debug,
                      "[GSTREAMER] Reference-frame invalidation requested for {}-{}; forcing IDR fallback",
                      request->first_frame_index,
                      request->last_frame_index);
            force_idr(pipeline.get());
          }
        });

    auto pause_handler = event_bus->register_handler<immer::box<events::PauseStreamEvent>>(
        [sess_id = video_session->session_id, secret = video_session->rtp_secret_payload, pipeline](
            const immer::box<events::PauseStreamEvent> &ev) {
          if (events::pause_event_matches(sess_id, secret, *ev)) {
            logs::log(logs::debug, "[GSTREAMER] Pausing pipeline: {}", sess_id);

            /**
             * Unfortunately here we can't just pause the pipeline,
             * when a pipeline will be resumed there are a lot of breaking changes
             * like:
             *  - Client IP:PORT
             *  - AES key and IV for encrypted payloads
             *  - Client resolution, framerate, and encoding
             *
             *  The only solution is to kill the pipeline and re-create it again
             * when a resume happens
             */

            gst_element_send_event(pipeline.get(), gst_event_new_eos());
          }
        });

    auto switch_producer_handler = event_bus->register_handler<immer::box<events::SwitchStreamProducerEvents>>(
        [sess_id = video_session->session_id,
         pipeline](const immer::box<events::SwitchStreamProducerEvents> &switch_ev) {
          if (switch_ev->session_id == sess_id) {
            logs::log(logs::debug,
                      "[GSTREAMER] Switching video producer pipeline for {} to {}",
                      sess_id,
                      switch_ev->interpipe_src_id);
            /* Grab a reference to the interpipesrc */
            auto pipe_name = fmt::format("interpipesrc_{}_video", sess_id);
            if (auto src = gst_bin_get_by_name(GST_BIN(pipeline.get()), pipe_name.c_str())) {
              /* Perform the switch */
              auto video_interpipe = fmt::format("{}_video", switch_ev->interpipe_src_id);
              g_object_set(src, "listen-to", video_interpipe.c_str(), nullptr);
              gst_object_unref(src);
            } else {
              logs::log(logs::error, "[GSTREAMER] Failed to get video interpipesrc for {}", sess_id);
            }
          }
        });

    auto stop_handler = event_bus->register_handler<immer::box<events::StopStreamEvent>>(
        [sess_id = video_session->session_id, pipeline](const immer::box<events::StopStreamEvent> &ev) {
          if (ev->session_id == sess_id) {
            logs::log(logs::debug, "[GSTREAMER] Stopping pipeline: {}", sess_id);
            gst_element_send_event(pipeline.get(), gst_event_new_eos());
          }
        });

    return immer::array<immer::box<events::EventBusHandlers>>{std::move(idr_handler),
                                                              std::move(fec_status_handler),
                                                              std::move(loss_stats_handler),
                                                              std::move(invalidate_handler),
                                                              std::move(pause_handler),
                                                              std::move(switch_producer_handler),
                                                              std::move(stop_handler)};
  });
}

/**
 * Start AUDIO pipeline
 */
void start_streaming_audio(immer::box<events::AudioSession> audio_session,
                           const std::shared_ptr<events::EventBusType> &event_bus,
                           std::string client_ip,
                           unsigned short client_port,
                           std::shared_ptr<udp::socket> audio_socket,
                           const std::string &sink_name,
                           const std::string &server_name) {
  auto pipeline = fmt::format(
      fmt::runtime(audio_session->gst_pipeline),
      fmt::arg("session_id", audio_session->session_id),
      fmt::arg("channels", audio_session->audio_mode.channels),
      fmt::arg("bitrate", audio_session->audio_mode.bitrate),
      // TODO: opusenc hardcodes those two
      // https://gitlab.freedesktop.org/gstreamer/gstreamer/-/blob/1.24.6/subprojects/gst-plugins-base/ext/opus/gstopusenc.c#L661-666
      fmt::arg("streams", audio_session->audio_mode.streams),
      fmt::arg("coupled_streams", audio_session->audio_mode.coupled_streams),
      fmt::arg("sink_name", sink_name),
      fmt::arg("server_name", server_name),
      fmt::arg("packet_duration", audio_session->packet_duration),
      fmt::arg("aes_key", audio_session->aes_key),
      fmt::arg("aes_iv", audio_session->aes_iv),
      fmt::arg("encrypt", audio_session->encrypt_audio),
      fmt::arg("client_port", client_port),
      fmt::arg("client_ip", client_ip),
      fmt::arg("host_port", audio_session->port));
  logs::log(logs::debug, "Starting audio pipeline: \n{}", pipeline);

  std::shared_ptr<custom_sink::UDPSink> udp_sink = std::make_shared<custom_sink::UDPSink>(custom_sink::UDPSink{
      .socket = audio_socket,
      .client_endpoint = std::make_shared<udp::endpoint>(boost::asio::ip::make_address(client_ip), client_port)});

  auto secret = audio_session->rtp_secret_payload;
  run_pipeline(pipeline, [session_id = audio_session->session_id, secret, udp_sink, event_bus](auto pipeline) {
    if (auto app_sink_el = gst_bin_get_by_name(GST_BIN(pipeline.get()), "wolf_udp_sink")) {
      logs::log(logs::debug, "Setting up wolf_udp_sink");
      g_assert(GST_IS_APP_SINK(app_sink_el));
      custom_sink::configure_appsink(app_sink_el, udp_sink.get());
      gst_object_unref(app_sink_el);
    }

    auto pause_handler = event_bus->register_handler<immer::box<events::PauseStreamEvent>>(
        [session_id, secret, pipeline](const immer::box<events::PauseStreamEvent> &ev) {
          if (events::pause_event_matches(session_id, secret, *ev)) {
            logs::log(logs::debug, "[GSTREAMER] Pausing pipeline: {}", session_id);

            /**
             * Unfortunately here we can't just pause the pipeline,
             * when a pipeline will be resumed there are a lot of breaking changes
             * like:
             *  - Client IP:PORT
             *  - AES key and IV for encrypted payloads
             *  - Client resolution, framerate, and encoding
             *
             *  The only solution is to kill the pipeline and re-create it again
             * when a resume happens
             */

            gst_element_send_event(pipeline.get(), gst_event_new_eos());
          }
        });

    auto switch_producer_handler = event_bus->register_handler<immer::box<events::SwitchStreamProducerEvents>>(
        [session_id, pipeline](const immer::box<events::SwitchStreamProducerEvents> &switch_ev) {
          if (switch_ev->session_id == session_id) {
            logs::log(logs::debug,
                      "[GSTREAMER] Switching audio producer for {} to {}",
                      session_id,
                      switch_ev->interpipe_src_id);

            auto pipe_name = fmt::format("interpipesrc_{}_audio", session_id);
            if (auto src = gst_bin_get_by_name(GST_BIN(pipeline.get()), pipe_name.c_str())) {
              /* Perform the switch */
              auto audio_interpipe = fmt::format("{}_audio", switch_ev->interpipe_src_id);
              g_object_set(src, "listen-to", audio_interpipe.c_str(), nullptr);
              gst_object_unref(src);
            } else {
              logs::log(logs::error, "[GSTREAMER] Failed to get audio interpipesrc for {}", session_id);
            }
          }
        });

    auto stop_handler = event_bus->register_handler<immer::box<events::StopStreamEvent>>(
        [session_id, pipeline](const immer::box<events::StopStreamEvent> &ev) {
          if (ev->session_id == session_id) {
            logs::log(logs::debug, "[GSTREAMER] Stopping pipeline: {}", session_id);
            gst_element_send_event(pipeline.get(), gst_event_new_eos());
          }
        });

    return immer::array<immer::box<events::EventBusHandlers>>{std::move(pause_handler),
                                                              std::move(switch_producer_handler),
                                                              std::move(stop_handler)};
  });
}

} // namespace streaming

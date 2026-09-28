#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_container_properties.hpp>
#include <catch2/matchers/catch_matchers_contains.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <catch2/matchers/catch_matchers_vector.hpp>

using Catch::Matchers::Equals;

#include "video_fec_reference.hpp"
#include <gst-plugin/audio.hpp>
#include <gst-plugin/video.hpp>
#include <moonlight/fec.hpp>
#include <streaming/streaming.hpp>
#include <string>

using namespace std::string_literals;

/* UTILS */

static std::pair<char * /* data */, unsigned long /* size */> copy_buffer_data(GstBuffer *buf) {
  auto size = gst_buffer_get_size(buf);
  auto *res = new char[size];

  GstMapInfo info;
  gst_buffer_map(buf, &info, GST_MAP_READ);
  std::copy(info.data, info.data + size, res);
  gst_buffer_unmap(buf, &info);

  return {res, size};
}

static audio::AudioRTPHeaders *get_rtp_audio_from_buf(GstBuffer *buf) {
  return (audio::AudioRTPHeaders *)copy_buffer_data(buf).first;
}

static std::string get_str_from_buf(GstBuffer *buf) {
  auto copy = copy_buffer_data(buf);
  return {copy.first, copy.second};
}

static int get_buf_refcount(GstBuffer *buf) {
  return buf->mini_object.refcount;
}

class GStreamerTestsFixture {
public:
  GStreamerTestsFixture() {
    gst_init(nullptr, nullptr);
    moonlight::fec::init();
  }
};

/*
 * BASE UTILS
 */

TEST_CASE_METHOD(GStreamerTestsFixture, "Basic utils", "[GSTPlugin]") {
  auto buffer = gst_buffer_new_and_fill(10, 0);
  REQUIRE_THAT(gst_buffer_copy_content(buffer),
               Catch::Matchers::SizeIs(10) && Equals(std::vector<unsigned char>{0, 0, 0, 0, 0, 0, 0, 0, 0, 0}));
  /* Cleanup */
  REQUIRE(get_buf_refcount(buffer) == 1);
  gst_buffer_unref(buffer);

  auto payload = "char array"s;
  buffer = gst_buffer_new_and_fill(10, payload.c_str());
  REQUIRE_THAT(
      gst_buffer_copy_content(buffer),
      Catch::Matchers::SizeIs(payload.size()) && Equals(std::vector<unsigned char>(payload.begin(), payload.end())));
  REQUIRE_THAT(get_str_from_buf(buffer), Catch::Matchers::SizeIs(payload.size()) && Equals(payload));

  /* Cleanup */
  REQUIRE(get_buf_refcount(buffer) == 1);
  gst_buffer_unref(buffer);
}

TEST_CASE_METHOD(GStreamerTestsFixture, "Encrypt GstBuffer", "[GSTPlugin]") {
  auto payload = gst_buffer_new_and_fill(10, "$A PAYLOAD");
  auto aes_key = "0123456789012345"s;
  auto aes_iv = "12345678"s;
  auto cur_seq_number = 0;

  auto iv_str = derive_iv(aes_iv, cur_seq_number);

  REQUIRE_THAT(iv_str, Equals("\000\274aN\000\000\000\000\000\000\000\000\000\000\000\000"s));

  auto encrypted = encrypt_payload(aes_key, iv_str, payload);
  auto encrypted_str = get_str_from_buf(encrypted);

  auto decrypted = crypto::aes_decrypt_cbc(encrypted_str, aes_key, iv_str, true);
  REQUIRE_THAT(gst_buffer_copy_content(payload),
               Equals(std::vector<unsigned char>(decrypted.begin(), decrypted.end())));

  /* Cleanup */
  REQUIRE(get_buf_refcount(payload) == 1);
  gst_buffer_unref(payload);
}

/*
 * VIDEO
 */
TEST_CASE_METHOD(GStreamerTestsFixture, "RTP VIDEO Splits", "[GSTPlugin]") {
  auto rtpmoonlightpay = (gst_rtp_moonlight_pay_video *)g_object_new(gst_TYPE_rtp_moonlight_pay_video, nullptr);

  auto payload_str = "Never gonna give you up\n"
                     "Never gonna let you down\n"
                     "Never gonna run around and desert you\n"
                     "Never gonna make you cry\n"
                     "Never gonna say goodbye\n"
                     "Never gonna tell a lie and hurt you"s;

  auto rtp_header_size = (int)sizeof(gst_moonlight_video::VideoRTPHeaders);
  auto rtp_payload_header_size = 8; // 017charss
  rtpmoonlightpay->payload_size = 32;
  rtpmoonlightpay->fec_percentage = 50;
  rtpmoonlightpay->add_padding = false;

  auto payload_buf = gst_buffer_new_and_fill(payload_str.size(), payload_str.c_str());
  auto rtp_packets = gst_moonlight_video::split_into_rtp(rtpmoonlightpay, payload_buf);

  auto payload_expected_packets = std::ceil((float)(payload_str.size() + rtp_payload_header_size) /
                                            ((float)rtpmoonlightpay->payload_size - MAX_RTP_HEADER_SIZE));
  auto fec_expected_packets = std::ceil(payload_expected_packets * ((double)rtpmoonlightpay->fec_percentage / 100));

  REQUIRE(gst_buffer_list_length(rtp_packets) == payload_expected_packets + fec_expected_packets);

  std::string returned_payload = ""s;
  for (auto i = 0; i < payload_expected_packets; i++) {
    auto buf = gst_buffer_copy_content(gst_buffer_list_get(rtp_packets, i), rtp_header_size);
    returned_payload +=
        std::string(buf.begin() + (long)(i == 0 ? sizeof(gst_moonlight_video::VideoShortHeader) : 0), buf.end());
  }
  REQUIRE_THAT(returned_payload, Equals(payload_str));

  SECTION("Multi block FEC") {
    auto payload_buf_blocks = gst_buffer_new_and_fill(payload_str.size(), payload_str.c_str());
    auto rtp_packets_blocks = gst_moonlight_video::generate_rtp_packets(*rtpmoonlightpay, payload_buf_blocks);
    auto final_packets = gst_moonlight_video::generate_fec_multi_blocks(rtpmoonlightpay,
                                                                        rtp_packets_blocks,
                                                                        (int)payload_expected_packets,
                                                                        payload_buf_blocks,
                                                                        3);

    REQUIRE(gst_buffer_list_length(final_packets) ==
            payload_expected_packets + fec_expected_packets - 1); // TODO: why one less?

    auto first_payload = gst_buffer_copy_content(gst_buffer_list_get(final_packets, 0), rtp_header_size);
    REQUIRE_THAT(std::string(first_payload.begin(), first_payload.end()), Equals(payload_str.substr(0, 16)));
    // TODO: proper check content and FEC
    gst_buffer_list_unref(final_packets);
    gst_buffer_unref(payload_buf_blocks);
  }

  /* Cleanup */
  REQUIRE(GST_OBJECT_REFCOUNT(rtpmoonlightpay) == 1);
  g_object_unref(rtpmoonlightpay);
  gst_buffer_list_unref(rtp_packets);
  REQUIRE(get_buf_refcount(payload_buf) == 1);
  gst_buffer_unref(payload_buf);
}

TEST_CASE_METHOD(GStreamerTestsFixture, "Multi-block video FEC has recoverable block boundaries", "[GSTPlugin]") {
  auto rtpmoonlightpay = (gst_rtp_moonlight_pay_video *)g_object_new(gst_TYPE_rtp_moonlight_pay_video, nullptr);
  rtpmoonlightpay->payload_size = MAX_RTP_HEADER_SIZE + 10;
  rtpmoonlightpay->fec_percentage = 50;
  rtpmoonlightpay->min_required_fec_packets = 2;

  auto payload = gst_buffer_new_and_fill(60, 0x5a);
  GST_BUFFER_PTS(payload) = 2 * GST_SECOND;
  auto packets = gst_moonlight_video::generate_rtp_packets(*rtpmoonlightpay, payload);
  REQUIRE(gst_buffer_list_length(packets) == 6);

  auto final_packets = gst_moonlight_video::generate_fec_multi_blocks(rtpmoonlightpay, packets, 6, payload, 3);
  REQUIRE(gst_buffer_list_length(final_packets) == 12);

  for (int block = 0; block < 3; ++block) {
    const auto block_offset = block * 4;
    auto first = gst_buffer_copy_content(gst_buffer_list_get(final_packets, block_offset));
    auto last = gst_buffer_copy_content(gst_buffer_list_get(final_packets, block_offset + 1));
    const auto *first_header = reinterpret_cast<const gst_moonlight_video::VideoRTPHeaders *>(first.data());
    const auto *last_header = reinterpret_cast<const gst_moonlight_video::VideoRTPHeaders *>(last.data());

    REQUIRE(first_header->packet.flags == (FLAG_CONTAINS_PIC_DATA | FLAG_SOF));
    REQUIRE(last_header->packet.flags == (FLAG_CONTAINS_PIC_DATA | FLAG_EOF));
    REQUIRE(first_header->packet.multiFecBlocks == ((block << 4) | (2 << 6)));
    REQUIRE(last_header->packet.multiFecBlocks == ((block << 4) | (2 << 6)));
    REQUIRE(first_header->packet.streamPacketIndex == static_cast<std::uint32_t>(block_offset << 8));
    REQUIRE(last_header->packet.streamPacketIndex == static_cast<std::uint32_t>((block_offset + 1) << 8));
    REQUIRE(boost::endian::big_to_native(first_header->rtp.timestamp) == 180000);
    REQUIRE(boost::endian::big_to_native(last_header->rtp.timestamp) == 180000);
  }

  gst_buffer_list_unref(final_packets);
  gst_buffer_unref(payload);
  g_object_unref(rtpmoonlightpay);
}

TEST_CASE_METHOD(GStreamerTestsFixture,
                 "Video FEC workspace preserves wire bytes and recovery",
                 "[GSTPlugin][FECWorkspace]") {
  using namespace gst_moonlight_video;
  // Exercise short/exact final payloads, all four block counts, fallback without
  // FEC, changing FEC geometry, and the 16-bit RTP sequence wrap boundary.
  for (bool padding : {false, true}) {
    for (int percentage : {0, 5, 20, 50}) {
      for (int payload_bytes : {1, 40, 41, 1000, 13000, 25000, 39000, 50000}) {
        for (bool delta : {false, true}) {
          CAPTURE(padding, percentage, payload_bytes, delta);
          auto make_payloader = [&]() {
            auto pay = (gst_rtp_moonlight_pay_video *)g_object_new(gst_TYPE_rtp_moonlight_pay_video, nullptr);
            pay->payload_size = 64;
            pay->add_padding = padding;
            pay->fec_percentage = percentage;
            pay->cur_seq_number = 65530;
            pay->frame_num = 42;
            return std::unique_ptr<gst_rtp_moonlight_pay_video, decltype(&g_object_unref)>(pay, g_object_unref);
          };
          auto actual_pay = make_payloader();
          auto reference_pay = make_payloader();
          std::vector<unsigned char> payload(payload_bytes);
          for (int i = 0; i < payload_bytes; ++i) {
            payload[i] = static_cast<unsigned char>(i * 37 + i / 251);
          }
          auto input = std::unique_ptr<GstBuffer, decltype(&gst_buffer_unref)>(
              gst_buffer_new_and_fill(payload.size(), reinterpret_cast<const char *>(payload.data())),
              gst_buffer_unref);
          GST_BUFFER_PTS(input.get()) = 2 * GST_SECOND;
          GST_BUFFER_DTS(input.get()) = GST_SECOND;
          GST_BUFFER_DURATION(input.get()) = GST_SECOND / 60;
          GST_BUFFER_OFFSET(input.get()) = 42;
          GST_BUFFER_OFFSET_END(input.get()) = 43;
          if (delta) {
            GST_BUFFER_FLAG_SET(input.get(), GST_BUFFER_FLAG_DELTA_UNIT);
          }
          auto actual = std::unique_ptr<GstBufferList, decltype(&gst_buffer_list_unref)>(
              split_into_rtp(actual_pay.get(), input.get()),
              gst_buffer_list_unref);
          auto reference = std::unique_ptr<GstBufferList, decltype(&gst_buffer_list_unref)>(
              video_fec_reference::split(reference_pay.get(), input.get()),
              gst_buffer_list_unref);
          const int count = gst_buffer_list_length(actual.get());
          REQUIRE(count == gst_buffer_list_length(reference.get()));
          REQUIRE(actual_pay->cur_seq_number == reference_pay->cur_seq_number);
          REQUIRE(actual_pay->cur_seq_number == 65530 + count);
          REQUIRE(actual_pay->frame_num == 43);
          REQUIRE(get_buf_refcount(input.get()) >= 1);
          for (int i = 0; i < count; ++i) {
            auto a = gst_buffer_list_get(actual.get(), i);
            auto r = gst_buffer_list_get(reference.get(), i);
            REQUIRE(gst_buffer_copy_content(a) == gst_buffer_copy_content(r));
            REQUIRE(GST_BUFFER_PTS(a) == GST_BUFFER_PTS(r));
            REQUIRE(GST_BUFFER_DTS(a) == GST_BUFFER_DTS(r));
            REQUIRE(GST_BUFFER_DURATION(a) == GST_BUFFER_DURATION(r));
            REQUIRE(GST_BUFFER_OFFSET(a) == GST_BUFFER_OFFSET(r));
            REQUIRE(GST_BUFFER_OFFSET_END(a) == GST_BUFFER_OFFSET_END(r));
            auto bytes = gst_buffer_copy_content(a);
            auto header = reinterpret_cast<const VideoRTPHeaders *>(bytes.data());
            REQUIRE(boost::endian::big_to_native(header->rtp.sequenceNumber) == static_cast<uint16_t>(65530 + i));
            REQUIRE(boost::endian::big_to_native(header->rtp.timestamp) == 180000);
            REQUIRE(header->packet.frameIndex == 42);
          }
          const int shard_payload = actual_pay->payload_size - MAX_RTP_HEADER_SIZE;
          const int data_count = (payload_bytes + sizeof(VideoShortHeader) + shard_payload - 1) / shard_payload;
          const int blocks = percentage > 0 ? required_fec_blocks(*actual_pay, data_count) : 0;
          const int per_block = blocks ? (data_count + blocks - 1) / blocks : data_count;
          int offset = 0;
          std::vector<unsigned char> restored_payload;
          for (int block = 0; block < std::max(1, blocks); ++block) {
            const int data = std::min(per_block, data_count - block * per_block);
            const auto geometry = determine_split(*actual_pay, data);
            const int parity = blocks ? geometry.parity_shards : 0;
            const int width = geometry.block_size;
            std::vector<std::vector<unsigned char>> shards(data + parity, std::vector<unsigned char>(width, 0));
            for (int i = 0; i < data + parity; ++i) {
              const auto bytes = gst_buffer_copy_content(gst_buffer_list_get(actual.get(), offset + i));
              REQUIRE(bytes.size() <= width);
              std::copy(bytes.begin(), bytes.end(), shards[i].begin());
              if (i < data) {
                const auto *header = reinterpret_cast<const VideoRTPHeaders *>(bytes.data());
                REQUIRE(header->packet.flags ==
                        (FLAG_CONTAINS_PIC_DATA | (i == 0 ? FLAG_SOF : 0) | (i == data - 1 ? FLAG_EOF : 0)));
                REQUIRE(header->packet.streamPacketIndex == static_cast<uint32_t>((65530 + offset + i) << 8));
                REQUIRE(header->packet.multiFecBlocks == (blocks ? ((block << 4) | ((blocks - 1) << 6)) : 0));
                restored_payload.insert(restored_payload.end(), bytes.begin() + sizeof(VideoRTPHeaders), bytes.end());
              }
            }
            if (parity >= 2 && data >= 2) {
              const auto originals = shards;
              std::vector<unsigned char *> pointers;
              for (auto &shard : shards) {
                pointers.push_back(shard.data());
              }
              std::vector<unsigned char> marks(data + parity, 0);
              // Lose both block boundary data packets, including a short final shard.
              for (int missing : {0, data - 1}) {
                marks[missing] = 1;
                std::fill(shards[missing].begin(), shards[missing].end(), 0);
              }
              auto rs = moonlight::fec::create(data, parity);
              REQUIRE(moonlight::fec::decode(rs.get(), pointers.data(), marks.data(), data + parity, width) == 0);
              for (int missing : {0, data - 1}) {
                // Parity RTP headers are rewritten after encoding, so on-wire
                // shards reconstruct payload, not the original parity-covered header.
                REQUIRE(std::vector<unsigned char>(shards[missing].begin() + sizeof(VideoRTPHeaders),
                                                   shards[missing].end()) ==
                        std::vector<unsigned char>(originals[missing].begin() + sizeof(VideoRTPHeaders),
                                                   originals[missing].end()));
              }
            }
            offset += data + parity;
          }
          REQUIRE(offset == count);
          const auto *short_header = reinterpret_cast<const VideoShortHeader *>(restored_payload.data());
          REQUIRE(short_header->header_type == 1);
          REQUIRE(short_header->frame_type == (delta ? 1 : 2));
          const int remainder = (payload_bytes + sizeof(VideoShortHeader)) % shard_payload;
          REQUIRE(short_header->last_payload_len == (remainder ? remainder : shard_payload));
          REQUIRE(std::equal(payload.begin(), payload.end(), restored_payload.begin() + sizeof(VideoShortHeader)));
          const auto meaningful_size = payload_bytes + sizeof(VideoShortHeader);
          REQUIRE(restored_payload.size() == (padding ? data_count * shard_payload : meaningful_size));
          REQUIRE(std::all_of(restored_payload.begin() + meaningful_size, restored_payload.end(), [](auto b) {
            return b == 0;
          }));
          REQUIRE(gst_buffer_copy_content(input.get()) == payload);
        }
      }
    }
  }
}

TEST_CASE_METHOD(GStreamerTestsFixture, "Video RTP timestamps use the 90 kHz clock", "[GSTPlugin]") {
  auto rtpmoonlightpay = (gst_rtp_moonlight_pay_video *)g_object_new(gst_TYPE_rtp_moonlight_pay_video, nullptr);
  rtpmoonlightpay->payload_size = MAX_RTP_HEADER_SIZE + 10;
  auto payload = gst_buffer_new_and_fill(20, 0x01);
  GST_BUFFER_PTS(payload) = GST_SECOND + GST_SECOND / 2;

  auto packets = gst_moonlight_video::generate_rtp_packets(*rtpmoonlightpay, payload);
  REQUIRE(gst_buffer_list_length(packets) == 2);
  for (guint i = 0; i < gst_buffer_list_length(packets); ++i) {
    auto bytes = gst_buffer_copy_content(gst_buffer_list_get(packets, i));
    const auto *header = reinterpret_cast<const gst_moonlight_video::VideoRTPHeaders *>(bytes.data());
    REQUIRE(boost::endian::big_to_native(header->rtp.timestamp) == 135000);
  }

  gst_buffer_list_unref(packets);
  gst_buffer_unref(payload);
  g_object_unref(rtpmoonlightpay);
}

TEST_CASE_METHOD(GStreamerTestsFixture, "Video sequence advances when FEC is disabled", "[GSTPlugin]") {
  auto rtpmoonlightpay = (gst_rtp_moonlight_pay_video *)g_object_new(gst_TYPE_rtp_moonlight_pay_video, nullptr);
  rtpmoonlightpay->payload_size = MAX_RTP_HEADER_SIZE + 10;
  rtpmoonlightpay->fec_percentage = 0;
  auto payload = gst_buffer_new_and_fill(12, 0x01);

  auto first = gst_moonlight_video::split_into_rtp(rtpmoonlightpay, payload);
  const auto first_count = gst_buffer_list_length(first);
  REQUIRE(rtpmoonlightpay->cur_seq_number == first_count);
  auto second = gst_moonlight_video::split_into_rtp(rtpmoonlightpay, payload);
  auto bytes = gst_buffer_copy_content(gst_buffer_list_get(second, 0));
  const auto *header = reinterpret_cast<const gst_moonlight_video::VideoRTPHeaders *>(bytes.data());
  REQUIRE(boost::endian::big_to_native(header->rtp.sequenceNumber) == first_count);

  gst_buffer_list_unref(first);
  gst_buffer_list_unref(second);
  gst_buffer_unref(payload);
  g_object_unref(rtpmoonlightpay);
}

TEST_CASE("Adaptive video FEC raises on loss and decays on clean feedback", "[Streaming]") {
  using clock = streaming::AdaptiveFecController::clock;
  const auto start = clock::time_point{};
  streaming::AdaptiveFecController controller(true, 20, start);

  REQUIRE(controller.desired_percentage() == 20);

  const wolf::core::events::VideoFecStatusEvent clean{
      .missing_packets = 0,
      .total_data_packets = 80,
      .total_parity_packets = 20,
  };
  controller.report(clean, start + std::chrono::seconds(5));
  REQUIRE(controller.desired_percentage() == 15);
  controller.report(clean, start + std::chrono::seconds(10));
  REQUIRE(controller.desired_percentage() == 10);

  controller.report(
      wolf::core::events::VideoFecStatusEvent{
          .missing_packets = 20,
          .total_data_packets = 80,
          .total_parity_packets = 20,
      },
      start + std::chrono::seconds(11));
  REQUIRE(controller.desired_percentage() == 45);
  controller.report(clean, start + std::chrono::seconds(16));
  REQUIRE(controller.desired_percentage() == 40);

  streaming::AdaptiveFecController disabled(false, 20, start);
  disabled.report(clean, start + std::chrono::hours(1));
  REQUIRE(disabled.desired_percentage() == 20);
}

TEST_CASE("Adaptive video bitrate backs off on loss and recovers gradually", "[Streaming]") {
  using clock = streaming::AdaptiveBitrateController::clock;
  const auto start = clock::time_point{};
  streaming::AdaptiveBitrateController controller(20000, start);

  const wolf::core::events::VideoFecStatusEvent light_loss{
      .missing_packets = 1,
      .total_data_packets = 80,
      .total_parity_packets = 20,
  };
  controller.report(light_loss, start);
  REQUIRE(controller.desired_bitrate_kbps(start) == 18000);

  const wolf::core::events::VideoFecStatusEvent heavy_loss{
      .missing_packets = 10,
      .total_data_packets = 80,
      .total_parity_packets = 20,
  };
  controller.report(heavy_loss, start + std::chrono::seconds(1));
  REQUIRE(controller.desired_bitrate_kbps(start + std::chrono::seconds(1)) == 12600);
  REQUIRE(controller.desired_bitrate_kbps(start + std::chrono::seconds(5)) == 12600);
  REQUIRE(controller.desired_bitrate_kbps(start + std::chrono::seconds(6)) == 13600);
  REQUIRE(controller.desired_bitrate_kbps(start + std::chrono::seconds(8)) == 14600);

  streaming::AdaptiveBitrateController legacy_controller(20000, start);
  const wolf::core::events::VideoLossStatsEvent near_realtime{.last_good_frame = 95};
  legacy_controller.report(near_realtime, 100, start);
  REQUIRE(legacy_controller.desired_bitrate_kbps(start) == 20000);

  const wolf::core::events::VideoLossStatsEvent lagging{.last_good_frame = 90};
  legacy_controller.report(lagging, 100, start);
  REQUIRE(legacy_controller.desired_bitrate_kbps(start) == 16000);
}

TEST_CASE_METHOD(GStreamerTestsFixture, "Video FEC block count respects the 255 shard limit", "[GSTPlugin]") {
  auto rtpmoonlightpay = (gst_rtp_moonlight_pay_video *)g_object_new(gst_TYPE_rtp_moonlight_pay_video, nullptr);
  rtpmoonlightpay->fec_percentage = 20;
  rtpmoonlightpay->min_required_fec_packets = 2;

  // 212 data + 43 parity shards fits exactly. The next data shard requires
  // another FEC block because 213 + 43 would be 256.
  REQUIRE(gst_moonlight_video::required_fec_blocks(*rtpmoonlightpay, 212) == 1);
  REQUIRE(gst_moonlight_video::required_fec_blocks(*rtpmoonlightpay, 213) == 2);

  // Four protocol blocks can carry 4 * 212 data shards at 20% FEC. Beyond
  // that, the caller must fall back to a consistent no-FEC frame.
  REQUIRE(gst_moonlight_video::required_fec_blocks(*rtpmoonlightpay, 848) == 4);
  REQUIRE(gst_moonlight_video::required_fec_blocks(*rtpmoonlightpay, 849) == 0);

  g_object_unref(rtpmoonlightpay);
}

TEST_CASE_METHOD(GStreamerTestsFixture, "Create RTP VIDEO packets", "[GSTPlugin]") {
  auto rtpmoonlightpay = (gst_rtp_moonlight_pay_video *)g_object_new(gst_TYPE_rtp_moonlight_pay_video, nullptr);

  rtpmoonlightpay->payload_size = 10 + MAX_RTP_HEADER_SIZE; // This will include 8bytes of payload header (017charss)
  rtpmoonlightpay->fec_percentage = 50;
  rtpmoonlightpay->add_padding = true;
  auto rtp_packet_size = rtpmoonlightpay->payload_size + sizeof(moonlight::NV_VIDEO_PACKET);
  auto rtp_header_size = (long)sizeof(gst_moonlight_video::VideoRTPHeaders);

  auto payload = gst_buffer_new_and_fill(10, "$A PAYLOAD");
  auto video_payload = gst_moonlight_video::prepend_video_header(*rtpmoonlightpay, payload);
  auto rtp_packets = gst_moonlight_video::generate_rtp_packets(*rtpmoonlightpay, video_payload);

  // 10 bytes of actual payload + 8 bytes of payload header
  // will be splitted in two RTP packets
  REQUIRE(gst_buffer_get_size(video_payload) == gst_buffer_get_size(payload) + 8); // Added 017charss
  REQUIRE(gst_buffer_list_length(rtp_packets) == 2);

  SECTION("First packet") {
    auto first_packet = gst_buffer_copy_content(gst_buffer_list_get(rtp_packets, 0));
    auto rtp_packet = reinterpret_cast<gst_moonlight_video::VideoRTPHeaders *>(first_packet.data());

    REQUIRE(rtp_packet->packet.flags == FLAG_CONTAINS_PIC_DATA + FLAG_SOF);
    REQUIRE(rtp_packet->packet.frameIndex == 0);
    REQUIRE(rtp_packet->packet.streamPacketIndex == 0);
    REQUIRE(rtp_packet->rtp.sequenceNumber == boost::endian::native_to_big((uint16_t)0));

    auto short_header =
        reinterpret_cast<gst_moonlight_video::VideoShortHeader *>(first_packet.data() + rtp_header_size);
    REQUIRE(short_header->frame_type == 2);
    REQUIRE(short_header->header_type == 1);
    REQUIRE(short_header->last_payload_len == 8);

    auto rtp_payload = std::string(
        first_packet.begin() + rtp_header_size + sizeof(gst_moonlight_video::VideoShortHeader),
        first_packet.end());
    REQUIRE_THAT("$A"s, Equals(rtp_payload));
  }

  SECTION("Second packet") {
    auto second_packet = gst_buffer_copy_content(gst_buffer_list_get(rtp_packets, 1));
    auto rtp_packet = reinterpret_cast<gst_moonlight_video::VideoRTPHeaders *>(second_packet.data());

    REQUIRE(rtp_packet->packet.flags == FLAG_CONTAINS_PIC_DATA + FLAG_EOF);
    REQUIRE(rtp_packet->packet.frameIndex == 0);
    REQUIRE(rtp_packet->packet.streamPacketIndex == 0x100);
    REQUIRE(rtp_packet->rtp.sequenceNumber == boost::endian::native_to_big((uint16_t)1));

    auto rtp_payload = std::string(second_packet.begin() + rtp_header_size, second_packet.end());
    REQUIRE_THAT(" PAYLOAD\0\0"s, Equals(rtp_payload));
  }

  SECTION("FEC") {
    gst_moonlight_video::generate_fec_packets(*rtpmoonlightpay, rtp_packets, payload);
    // Will append min_required_fec_packets to the original payload packets
    REQUIRE(gst_buffer_list_length(rtp_packets) == 4);

    SECTION("First packet (payload)") {
      auto first_packet = gst_buffer_copy_content(gst_buffer_list_get(rtp_packets, 0));

      auto rtp_packet = reinterpret_cast<gst_moonlight_video::VideoRTPHeaders *>(first_packet.data());

      REQUIRE(rtp_packet->packet.flags == FLAG_CONTAINS_PIC_DATA + FLAG_SOF);
      REQUIRE(rtp_packet->packet.frameIndex == 0);
      REQUIRE(rtp_packet->packet.streamPacketIndex == 0);
      REQUIRE(rtp_packet->rtp.sequenceNumber == boost::endian::native_to_big((uint16_t)0));

      // FEC additional info
      REQUIRE(rtp_packet->packet.fecInfo == 8390208);
      REQUIRE(rtp_packet->packet.multiFecBlocks == 0);
      REQUIRE(rtp_packet->packet.multiFecFlags == 0x10);

      auto short_header =
          reinterpret_cast<gst_moonlight_video::VideoShortHeader *>(first_packet.data() + rtp_header_size);
      REQUIRE(short_header->frame_type == 2);
      REQUIRE(short_header->header_type == 1);
      REQUIRE(short_header->last_payload_len == 8);

      auto rtp_payload = std::string(
          first_packet.begin() + rtp_header_size + sizeof(gst_moonlight_video::VideoShortHeader),
          first_packet.end());
      REQUIRE_THAT("$A"s, Equals(rtp_payload));
    }

    SECTION("Second packet (payload)") {
      auto second_packet = gst_buffer_copy_content(gst_buffer_list_get(rtp_packets, 1));
      auto rtp_packet = reinterpret_cast<gst_moonlight_video::VideoRTPHeaders *>(second_packet.data());

      REQUIRE(rtp_packet->packet.flags == FLAG_CONTAINS_PIC_DATA + FLAG_EOF);
      REQUIRE(rtp_packet->packet.frameIndex == 0);
      REQUIRE(rtp_packet->packet.streamPacketIndex == 0x100);
      REQUIRE(rtp_packet->rtp.sequenceNumber == boost::endian::native_to_big((uint16_t)1));

      // FEC additional info
      REQUIRE(rtp_packet->packet.fecInfo == 8394304);
      REQUIRE(rtp_packet->packet.multiFecBlocks == 0);
      REQUIRE(rtp_packet->packet.multiFecFlags == 0x10);

      auto rtp_payload = std::string(second_packet.begin() + rtp_header_size, second_packet.end());
      REQUIRE_THAT(" PAYLOAD\0\0"s, Equals(rtp_payload));
    }

    SECTION("Third packet (FEC)") {
      auto third_packet = gst_buffer_copy_content(gst_buffer_list_get(rtp_packets, 2));
      auto rtp_packet = reinterpret_cast<gst_moonlight_video::VideoRTPHeaders *>(third_packet.data());

      REQUIRE(rtp_packet->packet.frameIndex == 0);
      REQUIRE(rtp_packet->rtp.sequenceNumber == boost::endian::native_to_big((uint16_t)2));

      // FEC additional info
      REQUIRE(rtp_packet->packet.fecInfo == 8398400);
      REQUIRE(rtp_packet->packet.multiFecBlocks == 0);
      REQUIRE(rtp_packet->packet.multiFecFlags == 0x10);
    }

    SECTION("Fourth packet (FEC)") {
      auto fourth_packet = gst_buffer_copy_content(gst_buffer_list_get(rtp_packets, 3));
      auto rtp_packet = reinterpret_cast<gst_moonlight_video::VideoRTPHeaders *>(fourth_packet.data());

      REQUIRE(rtp_packet->packet.frameIndex == 0);
      REQUIRE(rtp_packet->rtp.sequenceNumber == boost::endian::native_to_big((uint16_t)3));

      // FEC additional info
      REQUIRE(rtp_packet->packet.fecInfo == 8402496);
      REQUIRE(rtp_packet->packet.multiFecBlocks == 0);
      REQUIRE(rtp_packet->packet.multiFecFlags == 0x10);
    }

    SECTION("REED SOLOMON") {
      auto data_shards = 2;
      auto parity_shards = 2;
      auto total_shards = data_shards + parity_shards;

      auto flatten_packets = gst_buffer_list_unfold(rtp_packets);
      auto packets_content = gst_buffer_copy_content(flatten_packets);

      std::vector<unsigned char *> packets_ptr(total_shards);
      for (int shard_idx = 0; shard_idx < total_shards; shard_idx++) {
        packets_ptr[shard_idx] = &packets_content.front() + (shard_idx * rtp_packet_size);
      }

      SECTION("If no package is marked nothing should change") {
        std::vector<unsigned char> marks = {0, 0, 0, 0};

        auto rs = moonlight::fec::create(data_shards, parity_shards);
        auto result =
            moonlight::fec::decode(rs.get(), &packets_ptr.front(), &marks.front(), total_shards, rtp_packet_size);

        REQUIRE(result == 0);
        REQUIRE_THAT(packets_content, Equals(gst_buffer_copy_content(flatten_packets)));
      }

      SECTION("Missing one packet should still lead to successfully reconstruct") {
        auto missing_pkt = std::vector<unsigned char>(rtp_packet_size);
        packets_ptr[0] = &missing_pkt[0];
        std::vector<unsigned char> marks = {1, 0, 0, 0};

        auto rs = moonlight::fec::create(data_shards, parity_shards);
        auto result =
            moonlight::fec::decode(rs.get(), &packets_ptr.front(), &marks.front(), total_shards, rtp_packet_size);

        REQUIRE(result == 0);

        // Here the packet headers will be wrongly reconstructed because we are manually
        // modifying the parity packets after creation
        // We can only check the packet payload here which should be correctly reconstructed
        auto pay_size = rtpmoonlightpay->payload_size - MAX_RTP_HEADER_SIZE;
        auto missing_pkt_payload = std::vector<unsigned char>(
            missing_pkt.begin() + sizeof(gst_moonlight_video::VideoRTPHeaders),
            missing_pkt.begin() + sizeof(gst_moonlight_video::VideoRTPHeaders) + pay_size);
        auto first_packet_pay_before_fec = gst_buffer_copy_content(gst_buffer_list_get(rtp_packets, 0),
                                                                   sizeof(gst_moonlight_video::VideoRTPHeaders),
                                                                   pay_size);

        REQUIRE_THAT(missing_pkt_payload, Equals(first_packet_pay_before_fec));
      }
    }
  }

  REQUIRE(GST_OBJECT_REFCOUNT(rtpmoonlightpay) == 1);
  g_object_unref(rtpmoonlightpay);
  REQUIRE(get_buf_refcount(payload) == 1);
  gst_buffer_unref(payload);
  gst_buffer_list_unref(rtp_packets);
  REQUIRE(get_buf_refcount(video_payload) == 1);
  gst_buffer_unref(video_payload);
}

/*
 * AUDIO
 */
TEST_CASE_METHOD(GStreamerTestsFixture, "Audio RTP packet creation", "[GSTPlugin]") {
  auto rtpmoonlightpay = std::shared_ptr<gst_rtp_moonlight_pay_audio>(
      (gst_rtp_moonlight_pay_audio *)g_object_new(gst_TYPE_rtp_moonlight_pay_audio, nullptr),
      g_object_unref);

  rtpmoonlightpay->encrypt = true;
  rtpmoonlightpay->aes_key = "0123456789012345";
  rtpmoonlightpay->aes_iv = "12345678";

  auto payload_str = "TUNZ TUNZ TUMP TUMP!"s;
  auto payload = gst_buffer_new_and_fill(payload_str.size(), payload_str.c_str());
  auto rtp_packets = audio::split_into_rtp(rtpmoonlightpay.get(), payload);

  REQUIRE(gst_buffer_list_length(rtp_packets) == 1);
  REQUIRE(rtpmoonlightpay->cur_seq_number == 1);
  auto first_pkt = gst_buffer_list_get(rtp_packets, 0);

  SECTION("First packet") {
    auto rtp_packet = get_rtp_audio_from_buf(first_pkt);

    REQUIRE(rtp_packet->rtp.ssrc == 0);
    REQUIRE(rtp_packet->rtp.packetType == 97);
    REQUIRE(rtp_packet->rtp.header == 0x80);
    REQUIRE(rtp_packet->rtp.sequenceNumber == 0);
    REQUIRE(rtp_packet->rtp.timestamp == 0);

    auto rtp_payload = gst_buffer_copy_content(first_pkt, sizeof(audio::AudioRTPHeaders));

    auto decrypted = crypto::aes_decrypt_cbc(std::string(rtp_payload.begin(), rtp_payload.end()),
                                             rtpmoonlightpay->aes_key,
                                             derive_iv(rtpmoonlightpay->aes_iv, rtpmoonlightpay->cur_seq_number - 1),
                                             true);
    REQUIRE_THAT(decrypted, Equals(payload_str));
  }

  rtp_packets = audio::split_into_rtp(rtpmoonlightpay.get(), payload);
  REQUIRE(gst_buffer_list_length(rtp_packets) == 1);
  REQUIRE(rtpmoonlightpay->cur_seq_number == 2);
  auto second_pkt = gst_buffer_list_get(rtp_packets, 0);

  SECTION("Second packet") {
    auto rtp_packet = get_rtp_audio_from_buf(second_pkt);

    REQUIRE(rtp_packet->rtp.ssrc == 0);
    REQUIRE(rtp_packet->rtp.packetType == 97);
    REQUIRE(rtp_packet->rtp.header == 0x80);
    REQUIRE(boost::endian::big_to_native(rtp_packet->rtp.sequenceNumber) == 1);
    REQUIRE(boost::endian::big_to_native(rtp_packet->rtp.timestamp) == 5);

    auto rtp_payload = gst_buffer_copy_content(second_pkt, sizeof(audio::AudioRTPHeaders));

    auto decrypted = crypto::aes_decrypt_cbc(std::string(rtp_payload.begin(), rtp_payload.end()),
                                             rtpmoonlightpay->aes_key,
                                             derive_iv(rtpmoonlightpay->aes_iv, rtpmoonlightpay->cur_seq_number - 1),
                                             true);
    REQUIRE_THAT(decrypted, Equals(payload_str));
  }

  rtp_packets = audio::split_into_rtp(rtpmoonlightpay.get(), payload);
  REQUIRE(gst_buffer_list_length(rtp_packets) == 1);
  REQUIRE(rtpmoonlightpay->cur_seq_number == 3);
  auto third_pkt = gst_buffer_list_get(rtp_packets, 0);

  SECTION("Third packet") {
    auto rtp_packet = get_rtp_audio_from_buf(third_pkt);

    REQUIRE(rtp_packet->rtp.ssrc == 0);
    REQUIRE(rtp_packet->rtp.packetType == 97);
    REQUIRE(rtp_packet->rtp.header == 0x80);
    REQUIRE(boost::endian::big_to_native(rtp_packet->rtp.sequenceNumber) == 2);
    REQUIRE(boost::endian::big_to_native(rtp_packet->rtp.timestamp) == 10);

    auto rtp_payload = gst_buffer_copy_content(third_pkt, sizeof(audio::AudioRTPHeaders));

    auto decrypted = crypto::aes_decrypt_cbc(std::string(rtp_payload.begin(), rtp_payload.end()),
                                             rtpmoonlightpay->aes_key,
                                             derive_iv(rtpmoonlightpay->aes_iv, rtpmoonlightpay->cur_seq_number - 1),
                                             true);
    REQUIRE_THAT(decrypted, Equals(payload_str));
  }

  /* When the 4th packet arrives, we'll also FEC encode all the previous and return
   * the data packet + 2 more FEC packets
   */
  rtp_packets = audio::split_into_rtp(rtpmoonlightpay.get(), payload);
  REQUIRE(gst_buffer_list_length(rtp_packets) == 3); // One data packet + 2 FEC packets
  REQUIRE(rtpmoonlightpay->cur_seq_number == 4);

  SECTION("FEC") {
    SECTION("First FEC packet") {
      auto fec_packet = (audio::AudioFECPacket *)copy_buffer_data((gst_buffer_list_get(rtp_packets, 1))).first;

      REQUIRE(fec_packet->rtp.ssrc == 0);
      REQUIRE(fec_packet->rtp.packetType == 127);
      REQUIRE(fec_packet->rtp.header == 0x80);
      REQUIRE(fec_packet->rtp.timestamp == 0);

      REQUIRE(boost::endian::big_to_native(fec_packet->rtp.sequenceNumber) == 3);
      REQUIRE(fec_packet->fec_header.payloadType == 97);
      REQUIRE(fec_packet->fec_header.ssrc == 0);
      REQUIRE(fec_packet->fec_header.fecShardIndex == 0);
    }

    SECTION("Second FEC packet") {
      auto fec_packet = (audio::AudioFECPacket *)copy_buffer_data((gst_buffer_list_get(rtp_packets, 2))).first;

      REQUIRE(fec_packet->rtp.ssrc == 0);
      REQUIRE(fec_packet->rtp.packetType == 127);
      REQUIRE(fec_packet->rtp.header == 0x80);
      REQUIRE(fec_packet->rtp.timestamp == 0);

      REQUIRE(boost::endian::big_to_native(fec_packet->rtp.sequenceNumber) == 4);
      REQUIRE(fec_packet->fec_header.payloadType == 97);
      REQUIRE(fec_packet->fec_header.ssrc == 0);
      REQUIRE(fec_packet->fec_header.fecShardIndex == 1);
    }
  }

  SECTION("REED SOLOMON") {
    auto packet_size = gst_buffer_get_size(gst_buffer_list_get(rtp_packets, 0));

    SECTION("If no package is marked nothing should change") {
      std::vector<unsigned char> marks = {0, 0, 0, 0, 0, 0};

      auto result = moonlight::fec::decode(rtpmoonlightpay->rs.get(),
                                           rtpmoonlightpay->packets_buffer,
                                           &marks.front(),
                                           AUDIO_TOTAL_SHARDS,
                                           packet_size);

      REQUIRE(result == 0);
    }

    SECTION("Missing one packet should still lead to successful reconstruct") {
      auto original_pkt = gst_buffer_copy_content(first_pkt, sizeof(audio::AudioRTPHeaders));
      auto missing_pkt = std::vector<unsigned char>(packet_size);
      std::copy(missing_pkt.begin(), missing_pkt.end(), rtpmoonlightpay->packets_buffer[0]);
      std::vector<unsigned char> marks = {1, 0, 0, 0, 0, 0};

      auto result = moonlight::fec::decode(rtpmoonlightpay->rs.get(),
                                           rtpmoonlightpay->packets_buffer,
                                           &marks.front(),
                                           AUDIO_TOTAL_SHARDS,
                                           packet_size);

      REQUIRE(result == 0);
      REQUIRE_THAT(std::string(rtpmoonlightpay->packets_buffer[0] + sizeof(audio::AudioRTPHeaders),
                               rtpmoonlightpay->packets_buffer[0] + packet_size),
                   Equals(std::string(original_pkt.begin(), original_pkt.end())));
    }
  }
}

TEST_CASE_METHOD(GStreamerTestsFixture, "Audio RTP without encryption", "[GSTPlugin]") {
  auto rtpmoonlightpay = std::shared_ptr<gst_rtp_moonlight_pay_audio>(
      (gst_rtp_moonlight_pay_audio *)g_object_new(gst_TYPE_rtp_moonlight_pay_audio, nullptr),
      g_object_unref);

  rtpmoonlightpay->encrypt = false;

  auto payload_str = "TUNZ TUNZ TUMP TUMP!"s;
  auto payload = gst_buffer_new_and_fill(payload_str.size(), payload_str.c_str());
  auto rtp_packets = audio::split_into_rtp(rtpmoonlightpay.get(), payload);

  REQUIRE(gst_buffer_list_length(rtp_packets) == 1);
  REQUIRE(rtpmoonlightpay->cur_seq_number == 1);
  auto first_pkt = gst_buffer_list_get(rtp_packets, 0);

  SECTION("First packet") {
    auto rtp_packet = get_rtp_audio_from_buf(first_pkt);

    REQUIRE(rtp_packet->rtp.ssrc == 0);
    REQUIRE(rtp_packet->rtp.packetType == 97);
    REQUIRE(rtp_packet->rtp.header == 0x80);
    REQUIRE(rtp_packet->rtp.sequenceNumber == 0);
    REQUIRE(rtp_packet->rtp.timestamp == 0);

    auto rtp_payload = gst_buffer_copy_content(first_pkt, sizeof(audio::AudioRTPHeaders));
    REQUIRE_THAT(std::string(rtp_payload.begin(), rtp_payload.end()), Equals(payload_str));
  }
}

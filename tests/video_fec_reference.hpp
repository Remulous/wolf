#pragma once

#include <gst-plugin/video.hpp>

// Frozen pre-optimization FEC assembly from fe51439c. Keep the concatenation
// path independent of production workspace assembly for byte-equivalence tests.
namespace video_fec_reference {
using namespace gst_moonlight_video;

static void legacy_generate_fec_packets(const gst_rtp_moonlight_pay_video &rtpmoonlightpay,
                                        GstBufferList *rtp_packets,
                                        GstBuffer *inbuf,
                                        int block_index = 0,
                                        int last_block_index = 0) {
  GstMapInfo info;
  auto blocks = determine_split(rtpmoonlightpay, gst_buffer_list_length(rtp_packets));
  const auto nr_shards = blocks.data_shards + blocks.parity_shards;
  const auto timestamp = get_rtp_timestamp(inbuf);

  if (nr_shards > DATA_SHARDS_MAX) {
    logs::log(logs::warning,
              "[GSTREAMER] Size of frame too large, {} packets is bigger than the max ({}); skipping FEC",
              nr_shards,
              DATA_SHARDS_MAX);
    return;
  }

  // Finalize data headers before Reed-Solomon encoding. Recovered packets must
  // contain the same per-block flags and stream indexes as packets sent on the
  // wire, otherwise Moonlight rejects the reconstructed block.
  for (int shard_idx = 0; shard_idx < blocks.data_shards; shard_idx++) {
    GstMapInfo data_info;
    auto data_pkt = gst_buffer_list_get(rtp_packets, shard_idx);
    gst_buffer_map(data_pkt, &data_info, GST_MAP_WRITE);
    update_fec_info(rtpmoonlightpay,
                    reinterpret_cast<VideoRTPHeaders *>(data_info.data),
                    shard_idx,
                    blocks.data_shards,
                    blocks.fec_percentage,
                    block_index,
                    last_block_index,
                    timestamp);
    gst_copy_timestamps(inbuf, data_pkt);
    gst_buffer_unmap(data_pkt, &data_info);
  }

  GstBuffer *rtp_payload = gst_buffer_list_unfold(rtp_packets);
  auto payload_size = (int)gst_buffer_get_size(rtp_payload);

  // pads rtp_payload to blocksize
  if (payload_size % blocks.block_size != 0) {
    GstBuffer *pad = gst_buffer_new_and_fill((blocks.data_shards * blocks.block_size) - payload_size, 0x00);
    rtp_payload = gst_buffer_append(rtp_payload, pad);
  }

  // Allocate space for FEC packets
  auto fec_buff = gst_buffer_new_and_fill((blocks.parity_shards * blocks.block_size), 0x00);
  rtp_payload = gst_buffer_append(rtp_payload, fec_buff);
  gst_buffer_map(rtp_payload, &info, GST_MAP_WRITE);

  // Reed Solomon encode the full stream of bytes
  auto rs = moonlight::fec::create(blocks.data_shards, blocks.parity_shards);
  std::vector<unsigned char *> ptr(nr_shards);
  for (int shard_idx = 0; shard_idx < nr_shards; shard_idx++) {
    ptr[shard_idx] = info.data + (shard_idx * blocks.block_size);
  }
  if (moonlight::fec::encode(rs.get(), &ptr.front(), nr_shards, blocks.block_size) != 0) {
    logs::log(logs::warning, "Error during video FEC encoding");
  }

  // Push back the newly created RTP packets with the FEC info
  for (int shard_idx = blocks.data_shards; shard_idx < nr_shards; shard_idx++) {
    auto position = shard_idx * blocks.block_size;
    auto rtp_packet = (VideoRTPHeaders *)(info.data + position);

    update_fec_info(rtpmoonlightpay,
                    rtp_packet,
                    shard_idx,
                    blocks.data_shards,
                    blocks.fec_percentage,
                    block_index,
                    last_block_index,
                    timestamp);

    GstBuffer *packet_buf = gst_buffer_new_allocate(nullptr, blocks.block_size, nullptr);
    gst_buffer_fill(packet_buf, 0, rtp_packet, blocks.block_size);
    gst_copy_timestamps(inbuf, packet_buf);
    gst_buffer_list_add(rtp_packets, packet_buf);
  }

  gst_buffer_unmap(rtp_payload, &info);
  gst_buffer_unref(rtp_payload);
}

static GstBufferList *split(gst_rtp_moonlight_pay_video *pay, GstBuffer *input) {
  auto full = prepend_video_header(*pay, input);
  auto packets = generate_rtp_packets(*pay, full);
  const int count = gst_buffer_list_length(packets);
  const int blocks = pay->fec_percentage > 0 ? required_fec_blocks(*pay, count) : 0;
  if (blocks > 1) {
    auto result = gst_buffer_list_new();
    const int per_block = (count + blocks - 1) / blocks;
    for (int block = 0; block < blocks; ++block) {
      auto part = gst_buffer_list_sub(packets, block * per_block, std::min((block + 1) * per_block, count));
      legacy_generate_fec_packets(*pay, part, input, block, (blocks - 1) << 6);
      const int part_count = gst_buffer_list_length(part);
      for (int i = 0; i < part_count; ++i) {
        gst_buffer_list_add(result, gst_buffer_copy(gst_buffer_list_get(part, i)));
      }
      pay->cur_seq_number += part_count;
      gst_buffer_list_unref(part);
    }
    gst_buffer_list_unref(packets);
    packets = result;
  } else {
    if (blocks == 1) {
      legacy_generate_fec_packets(*pay, packets, input);
    }
    pay->cur_seq_number += gst_buffer_list_length(packets);
  }
  ++pay->frame_num;
  gst_buffer_unref(full);
  return packets;
}
} // namespace video_fec_reference

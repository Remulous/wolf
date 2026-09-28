#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <control/control.hpp>

using Catch::Matchers::Equals;

#include <moonlight/control.hpp>
using namespace moonlight::control;

static std::string to_string(const ControlEncryptedPacket &packet) {
  return {(char *)&packet, packet.full_size()};
}

TEST_CASE("Control AES Encryption", "CONTROL") {
  // A bunch of packets taken from a real session

  std::string aes_key = "EDF04A215C4FBEA20934120C8480D855";

  SECTION("30 bytes") { // original packet: 01001A0000000000BF0EB6DA10E47C702EC8644EB87D9CF7B6FAC9FF75CA
    std::string payload = crypto::hex_to_str("020302000000");
    std::uint32_t seq = 0;
    auto encrypted_packet = *encrypt_packet(aes_key, seq, payload);
    REQUIRE_THAT(crypto::str_to_hex(to_string(encrypted_packet)),
                 Equals("01001A0000000000BF0EB6DA10E47C702EC8644EB87D9CF7B6FAC9FF75CA"));
    REQUIRE(boost::endian::little_to_native(encrypted_packet.seq) == seq);
    REQUIRE(boost::endian::little_to_native(encrypted_packet.header.length) ==
            sizeof(encrypted_packet.seq) + GCM_TAG_SIZE + payload.length());

    auto decrypted = decrypt_packet(encrypted_packet, aes_key);
    REQUIRE_THAT(decrypted, Equals(payload));
    REQUIRE_THAT(packet_type_to_str(((ControlPacket *)decrypted.data())->type), Equals("IDR_FRAME"));
  }

  SECTION("29 bytes") { // original packet: 010019000100000021DBB8DC0590AF3A2B20BCE5A347DE31D366E5B9C5"
    std::string payload = crypto::hex_to_str("0703010000");
    std::uint32_t seq = 1;
    auto encrypted_packet = *encrypt_packet(aes_key, seq, payload);
    REQUIRE_THAT(crypto::str_to_hex(to_string(encrypted_packet)),
                 Equals("010019000100000021DBB8DC0590AF3A2B20BCE5A347DE31D366E5B9C5"));
    REQUIRE(boost::endian::little_to_native(encrypted_packet.seq) == seq);
    REQUIRE(boost::endian::little_to_native(encrypted_packet.header.length) ==
            sizeof(encrypted_packet.seq) + GCM_TAG_SIZE + payload.length());

    auto decrypted = decrypt_packet(encrypted_packet, aes_key);
    REQUIRE_THAT(decrypted, Equals(payload));
    REQUIRE_THAT(packet_type_to_str(((ControlPacket *)decrypted.data())->type), Equals("START_B"));
  }

  SECTION("36 bytes") { // original packet: 0100200002000000220722FBADED58A03F2E8898F0F1DCB7C93F6235590618E4186AD990
    std::string payload = crypto::hex_to_str("000208000400000000000000");
    std::uint32_t seq = 2;
    auto encrypted_packet = *encrypt_packet(aes_key, seq, payload);
    REQUIRE_THAT(crypto::str_to_hex(to_string(encrypted_packet)),
                 Equals("0100200002000000220722FBADED58A03F2E8898F0F1DCB7C93F6235590618E4186AD990"));
    REQUIRE(boost::endian::little_to_native(encrypted_packet.seq) == seq);
    REQUIRE(boost::endian::little_to_native(encrypted_packet.header.length) ==
            sizeof(encrypted_packet.seq) + GCM_TAG_SIZE + payload.length());

    auto decrypted = decrypt_packet(encrypted_packet, aes_key);
    REQUIRE_THAT(decrypted, Equals(payload));
    REQUIRE_THAT(packet_type_to_str(((ControlPacket *)decrypted.data())->type), Equals("PERIODIC_PING"));
  }

  SECTION("46 bytes") { // original packet:
                        // 01002A00060000005A4D999FB2542F85BDD39D99F77EB825254569D2C04E21241B5CEC01BD3F93129718ECC1F153
    std::string payload = crypto::hex_to_str("060212000000000E05000000033400C00000059F0329");
    std::uint32_t seq = 6;
    auto encrypted_packet = *encrypt_packet(aes_key, seq, payload);
    REQUIRE_THAT(
        crypto::str_to_hex(to_string(encrypted_packet)),
        Equals("01002A00060000005A4D999FB2542F85BDD39D99F77EB825254569D2C04E21241B5CEC01BD3F93129718ECC1F153"));
    REQUIRE(boost::endian::little_to_native(encrypted_packet.seq) == seq);
    REQUIRE(boost::endian::little_to_native(encrypted_packet.header.length) ==
            sizeof(encrypted_packet.seq) + GCM_TAG_SIZE + payload.length());

    auto decrypted = decrypt_packet(encrypted_packet, aes_key);
    REQUIRE_THAT(decrypted, Equals(payload));
    REQUIRE_THAT(packet_type_to_str(((ControlPacket *)decrypted.data())->type), Equals("INPUT_DATA"));
  }
}

TEST_CASE("control joypad input packets") {
  std::string payload =
      crypto::hex_to_str("060222000000001E0C0000001A000000010014000010000000000000000000009C0000005500");

  auto input_data = (pkts::CONTROLLER_MULTI_PACKET *)payload.data();
  auto pressed_btns = input_data->button_flags | (input_data->buttonFlags2 << 16);

  REQUIRE(input_data->type == pkts::CONTROLLER_MULTI);
  REQUIRE(input_data->active_gamepad_mask == 1);
  REQUIRE(pressed_btns & pkts::CONTROLLER_BTN::A);
}

TEST_CASE("Control packet validation", "[CONTROL]") {
  const auto valid_plaintext = crypto::hex_to_str("020302000000");
  REQUIRE(is_valid_control_packet(valid_plaintext));
  REQUIRE_FALSE(is_valid_control_packet({}));
  REQUIRE_FALSE(is_valid_control_packet(valid_plaintext.substr(0, 5)));

  const auto aes_key = "EDF04A215C4FBEA20934120C8480D855";
  const auto encrypted = *encrypt_packet(aes_key, 1, valid_plaintext);
  auto bytes = to_string(encrypted);
  REQUIRE(is_valid_encrypted_control_packet(bytes));

  bytes.resize(sizeof(ControlPacket) + sizeof(std::uint32_t) + GCM_TAG_SIZE - 1);
  REQUIRE_FALSE(is_valid_encrypted_control_packet(bytes));
}

TEST_CASE("Control encryption preserves distinct sequence values", "[CONTROL]") {
  const auto aes_key = "EDF04A215C4FBEA20934120C8480D855";
  const auto payload = crypto::hex_to_str("020302000000");
  const auto first = *encrypt_packet(aes_key, 0, payload);
  const auto second = *encrypt_packet(aes_key, 1, payload);

  REQUIRE(boost::endian::little_to_native(first.seq) == 0);
  REQUIRE(boost::endian::little_to_native(second.seq) == 1);
  REQUIRE(to_string(first) != to_string(second));
}

TEST_CASE("Control session lookup retains an immutable client snapshot", "[CONTROL]") {
  auto session = wolf::core::events::StreamSession{.enet_secret_payload = 17, .session_id = 42, .ip = "192.0.2.1"};
  auto running_sessions = std::make_shared<immer::atom<immer::vector<wolf::core::events::StreamSession>>>(
      immer::vector<wolf::core::events::StreamSession>{session});
  control::enet_clients_map connected_clients;
  ENetPeer peer{};

  ENetEvent connect{};
  connect.type = ENET_EVENT_TYPE_CONNECT;
  connect.data = session.enet_secret_payload;
  auto connecting_session = control::get_current_session(connected_clients, running_sessions, session.ip, connect);
  REQUIRE(connecting_session);
  REQUIRE(connecting_session->get().session_id == session.session_id);

  connected_clients = connected_clients.set(&peer, connecting_session.value());
  ENetEvent receive{};
  receive.type = ENET_EVENT_TYPE_RECEIVE;
  receive.peer = &peer;
  auto established_session = control::get_current_session(connected_clients, running_sessions, session.ip, receive);
  REQUIRE(established_session);

  connected_clients = connected_clients.erase(&peer);
  REQUIRE(established_session->get().session_id == session.session_id);
  REQUIRE(established_session->get().aes_key == session.aes_key);
}

TEST_CASE("Input packet validation", "[CONTROL]") {
  const auto valid_input =
      crypto::hex_to_str("060222000000001E0C0000001A000000010014000010000000000000000000009C0000005500");
  REQUIRE(is_valid_input_packet(valid_input));

  auto truncated_input = valid_input;
  truncated_input.resize(sizeof(pkts::INPUT_PKT));
  REQUIRE_FALSE(is_valid_input_packet(truncated_input));
}

TEST_CASE("Video feedback control packet validation", "[CONTROL]") {
  ControlFrameFecStatusPacket status{};
  status.header = {.type = pkts::FRAME_FEC_STATUS,
                   .length = boost::endian::native_to_little(
                       static_cast<std::uint16_t>(sizeof(ControlFrameFecStatusPacket) - sizeof(ControlPacket)))};
  std::string status_bytes(reinterpret_cast<const char *>(&status), sizeof(status));
  REQUIRE(is_valid_frame_fec_status_packet(status_bytes));
  REQUIRE_FALSE(is_valid_frame_fec_status_packet(status_bytes.substr(0, status_bytes.size() - 1)));

  ControlLossStatsPacket loss_stats{};
  loss_stats.header = {.type = pkts::LOSS_STATS,
                       .length = boost::endian::native_to_little(
                           static_cast<std::uint16_t>(sizeof(ControlLossStatsPacket) - sizeof(ControlPacket)))};
  std::string loss_stats_bytes(reinterpret_cast<const char *>(&loss_stats), sizeof(loss_stats));
  REQUIRE(is_valid_loss_stats_packet(loss_stats_bytes));
  REQUIRE_FALSE(is_valid_loss_stats_packet(loss_stats_bytes.substr(0, loss_stats_bytes.size() - 1)));

  ControlInvalidateReferenceFramesPacket invalidation{};
  invalidation.header = {.type = pkts::INVALIDATE_REF_FRAMES,
                         .length = boost::endian::native_to_little(static_cast<std::uint16_t>(
                             sizeof(ControlInvalidateReferenceFramesPacket) - sizeof(ControlPacket)))};
  std::string invalidation_bytes(reinterpret_cast<const char *>(&invalidation), sizeof(invalidation));
  REQUIRE(is_valid_reference_frame_invalidation_packet(invalidation_bytes));
  REQUIRE_FALSE(
      is_valid_reference_frame_invalidation_packet(invalidation_bytes.substr(0, invalidation_bytes.size() - 1)));
}

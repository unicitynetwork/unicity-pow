// Copyright (c) 2025 The Unicity Foundation
// Distributed under the MIT software license

#include "network/notifications.hpp"
#include "util/uint.hpp"
#include <catch_amalgamated.hpp>
#include <atomic>

using namespace unicity;

TEST_CASE("NetworkNotifications: RAII subscription cleanup",
          "[network][notifications]") {
  auto &notifications = NetworkEvents();
  bool called = false;

  {
    auto sub = notifications.SubscribePeerDisconnected(
        [&](int, const std::string &, uint16_t, const std::string &, bool) { called = true; });

    notifications.NotifyPeerDisconnected(1, "127.0.0.1", 8333, "test", false);
    REQUIRE(called);
  } // subscription goes out of scope

  called = false;
  notifications.NotifyPeerDisconnected(2, "127.0.0.1", 8333, "test", false);
  REQUIRE(!called); // Callback no longer registered
}

TEST_CASE("NetworkNotifications: Multiple subscribers",
          "[network][notifications]") {
  auto &notifications = NetworkEvents();
  int count = 0;

  auto sub1 = notifications.SubscribePeerDisconnected(
      [&](int, const std::string &, uint16_t, const std::string &, bool) { count++; });
  auto sub2 = notifications.SubscribePeerDisconnected(
      [&](int, const std::string &, uint16_t, const std::string &, bool) { count++; });

  notifications.NotifyPeerDisconnected(1, "127.0.0.1", 8333, "test", false);
  REQUIRE(count == 2); // Both callbacks invoked
}

TEST_CASE("NetworkNotifications: Manual unsubscribe",
          "[network][notifications]") {
  auto &notifications = NetworkEvents();
  bool called = false;

  auto sub = notifications.SubscribePeerDisconnected(
      [&](int, const std::string &, uint16_t, const std::string &, bool) { called = true; });

  notifications.NotifyPeerDisconnected(1, "127.0.0.1", 8333, "test", false);
  REQUIRE(called);

  called = false;
  sub.Unsubscribe();

  notifications.NotifyPeerDisconnected(2, "127.0.0.1", 8333, "test", false);
  REQUIRE(!called); // Callback unsubscribed
}

TEST_CASE("NetworkNotifications: Move semantics", "[network][notifications]") {
  auto &notifications = NetworkEvents();
  bool called = false;

  auto sub1 = notifications.SubscribePeerDisconnected(
      [&](int, const std::string &, uint16_t, const std::string &, bool) { called = true; });

  // Move constructor
  auto sub2 = std::move(sub1);

  notifications.NotifyPeerDisconnected(1, "127.0.0.1", 8333, "test", false);
  REQUIRE(called); // sub2 still active

  called = false;

  // Move assignment
  NetworkNotifications::Subscription sub3;
  sub3 = std::move(sub2);

  notifications.NotifyPeerDisconnected(2, "127.0.0.1", 8333, "test", false);
  REQUIRE(called); // sub3 still active
}

TEST_CASE("NetworkNotifications: PeerConnected event",
          "[network][notifications]") {
  auto &notifications = NetworkEvents();
  int received_peer_id = -1;
  std::string received_address;
  std::string received_type;

  auto sub = notifications.SubscribePeerConnected(
      [&](int peer_id, const std::string &address, uint16_t,
          const std::string &connection_type) {
        received_peer_id = peer_id;
        received_address = address;
        received_type = connection_type;
      });

  notifications.NotifyPeerConnected(42, "192.168.1.1:8333", 8333, "outbound");

  REQUIRE(received_peer_id == 42);
  REQUIRE(received_address == "192.168.1.1:8333");
  REQUIRE(received_type == "outbound");
}

TEST_CASE("NetworkNotifications: InvalidHeader event",
          "[network][notifications]") {
  auto &notifications = NetworkEvents();
  int received_peer_id = -1;
  uint256 received_hash;
  std::string received_reason;

  auto sub = notifications.SubscribeInvalidHeader(
      [&](int peer_id, const uint256 &hash, const std::string &reason) {
        received_peer_id = peer_id;
        received_hash = hash;
        received_reason = reason;
      });

  uint256 test_hash;
  test_hash.SetHex("0000000000000000000000000000000000000000000000000000000000000001");

  notifications.NotifyInvalidHeader(123, test_hash, "invalid PoW");

  REQUIRE(received_peer_id == 123);
  REQUIRE(received_hash == test_hash);
  REQUIRE(received_reason == "invalid PoW");
}

TEST_CASE("NetworkNotifications: LowWorkHeaders event",
          "[network][notifications]") {
  auto &notifications = NetworkEvents();
  int received_peer_id = -1;
  size_t received_count = 0;
  std::string received_reason;

  auto sub = notifications.SubscribeLowWorkHeaders(
      [&](int peer_id, size_t count, const std::string &reason) {
        received_peer_id = peer_id;
        received_count = count;
        received_reason = reason;
      });

  notifications.NotifyLowWorkHeaders(456, 10, "insufficient work");

  REQUIRE(received_peer_id == 456);
  REQUIRE(received_count == 10);
  REQUIRE(received_reason == "insufficient work");
}

TEST_CASE("NetworkNotifications: InvalidBlock event",
          "[network][notifications]") {
  auto &notifications = NetworkEvents();
  int received_peer_id = -1;
  uint256 received_hash;
  std::string received_reason;

  auto sub = notifications.SubscribeInvalidBlock(
      [&](int peer_id, const uint256 &hash, const std::string &reason) {
        received_peer_id = peer_id;
        received_hash = hash;
        received_reason = reason;
      });

  uint256 test_hash;
  test_hash.SetHex("0000000000000000000000000000000000000000000000000000000000000002");

  notifications.NotifyInvalidBlock(789, test_hash, "invalid merkle root");

  REQUIRE(received_peer_id == 789);
  REQUIRE(received_hash == test_hash);
  REQUIRE(received_reason == "invalid merkle root");
}

TEST_CASE("NetworkNotifications: Misbehavior event",
          "[network][notifications]") {
  auto &notifications = NetworkEvents();
  int received_peer_id = -1;
  int received_penalty = 0;
  std::string received_reason;

  auto sub = notifications.SubscribeMisbehavior(
      [&](int peer_id, int penalty, const std::string &reason) {
        received_peer_id = peer_id;
        received_penalty = penalty;
        received_reason = reason;
      });

  notifications.NotifyMisbehavior(111, 50, "protocol violation");

  REQUIRE(received_peer_id == 111);
  REQUIRE(received_penalty == 50);
  REQUIRE(received_reason == "protocol violation");
}

TEST_CASE("NetworkNotifications: Multiple event types",
          "[network][notifications]") {
  auto &notifications = NetworkEvents();
  int disconnect_count = 0;
  int invalid_header_count = 0;
  int misbehavior_count = 0;

  auto sub1 = notifications.SubscribePeerDisconnected(
      [&](int, const std::string &, uint16_t, const std::string &, bool) {
        disconnect_count++;
      });

  auto sub2 = notifications.SubscribeInvalidHeader(
      [&](int, const uint256 &, const std::string &) {
        invalid_header_count++;
      });

  auto sub3 = notifications.SubscribeMisbehavior(
      [&](int, int, const std::string &) { misbehavior_count++; });

  notifications.NotifyPeerDisconnected(1, "127.0.0.1", 8333, "timeout", false);
  REQUIRE(disconnect_count == 1);
  REQUIRE(invalid_header_count == 0);
  REQUIRE(misbehavior_count == 0);

  uint256 hash;
  notifications.NotifyInvalidHeader(2, hash, "bad header");
  REQUIRE(disconnect_count == 1);
  REQUIRE(invalid_header_count == 1);
  REQUIRE(misbehavior_count == 0);

  notifications.NotifyMisbehavior(3, 100, "spam");
  REQUIRE(disconnect_count == 1);
  REQUIRE(invalid_header_count == 1);
  REQUIRE(misbehavior_count == 1);
}

TEST_CASE("NetworkNotifications: Singleton pattern",
          "[network][notifications]") {
  auto &notifications1 = NetworkEvents();
  auto &notifications2 = NetworkNotifications::Get();

  // Should be the same instance
  REQUIRE(&notifications1 == &notifications2);

  int count = 0;
  auto sub1 =
      notifications1.SubscribePeerDisconnected([&](int, const std::string &, uint16_t,
                                                    const std::string &, bool) {
        count++;
      });

  // Notify through different reference
  notifications2.NotifyPeerDisconnected(1, "127.0.0.1", 8333, "test", false);

  REQUIRE(count == 1);
}

TEST_CASE("NetworkNotifications: Thread safety", "[network][notifications]") {
  // This is a basic smoke test - proper thread safety would require
  // ThreadSanitizer or more complex concurrent testing
  auto &notifications = NetworkEvents();
  std::atomic<int> count{0};

  auto sub = notifications.SubscribePeerDisconnected(
      [&](int, const std::string &, uint16_t, const std::string &, bool) { count++; });

  // Single-threaded test - just verify it works
  for (int i = 0; i < 100; ++i) {
    notifications.NotifyPeerDisconnected(i, "127.0.0.1", 8333, "test", false);
  }

  REQUIRE(count == 100);
}

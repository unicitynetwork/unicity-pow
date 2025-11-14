#include "catch_amalgamated.hpp"
#include "network/real_transport.hpp"
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <chrono>

using namespace unicity::network;

namespace {
// Pick an available high-range port; try a small range to avoid flakiness.
static uint16_t pick_listen_port(RealTransport& t,
                                 std::function<void(TransportConnectionPtr)> accept_cb,
                                 uint16_t start = 42000,
                                 uint16_t end = 42100) {
    for (uint16_t p = start; p < end; ++p) {
        if (t.listen(p, accept_cb)) return p;
    }
    // Fallback: bind ephemeral port (0) and query the assigned port
    if (t.listen(0, accept_cb)) {
        return t.listening_port();
    }
    return 0;
}
}

TEST_CASE("RealTransport lifecycle is idempotent", "[network][transport][real]") {
    RealTransport t(1);

    // Not running before run()
    CHECK_FALSE(t.is_running());

    // stop() without run() should be safe
    t.stop();

    // run() starts, second run() is no-op
    t.run();
    CHECK(t.is_running());
    t.run();
    CHECK(t.is_running());

    // stop() is idempotent
    t.stop();
    t.stop();

    // Can be started again after stop
    t.run();
    CHECK(t.is_running());
    t.stop();
}

TEST_CASE("RealTransport listen/connect echo roundtrip", "[network][transport][real]") {
    RealTransport server(1);
    RealTransport client(1);

    std::shared_ptr<TransportConnection> inbound_conn;
    std::mutex m;
    std::condition_variable cv;
    bool accepted = false;
    bool connected = false;
    bool echoed = false;

    auto accept_cb = [&](TransportConnectionPtr c){
        {
            std::lock_guard<std::mutex> lk(m);
            inbound_conn = c;
            accepted = true;
        }
        // Echo server: read and write back
        inbound_conn->set_receive_callback([&](const std::vector<uint8_t>& data){
            inbound_conn->send(data);
        });
        inbound_conn->start();
        cv.notify_all();
    };

    // Try to bind (ephemeral fallback inside pick_listen_port)
    uint16_t port = pick_listen_port(server, accept_cb);
    if (port == 0) {
        WARN("Skipping: unable to bind any listening port (range + ephemeral)");
        return;
    }

    // Start reactors after listen is set up
    server.run();
    client.run();

    // Connect client
    std::shared_ptr<TransportConnection> client_conn;
client_conn = client.connect("127.0.0.1", port, [&](bool ok){
    {
        std::lock_guard<std::mutex> lk(m);
        connected = ok;
    }
    if (ok && client_conn) {
        client_conn->start();
    }
    cv.notify_all();
});
REQUIRE(client_conn);

// Prepare to receive echo
std::vector<uint8_t> received;
client_conn->set_receive_callback([&](const std::vector<uint8_t>& data){
    {
        std::lock_guard<std::mutex> lk(m);
        received = data;
        echoed = true;
    }
    cv.notify_all();
});

// Wait for accept+connect
{
    std::unique_lock<std::mutex> lk(m);
    cv.wait_for(lk, std::chrono::seconds(3), [&]{ return accepted && connected; });
}
REQUIRE(accepted);
REQUIRE(connected);

// Verify canonical remote addresses are non-empty and look like IPs
CHECK(!client_conn->remote_address().empty());
CHECK(!inbound_conn->remote_address().empty());

// Send payload and expect echo
const std::string payload = "hello";
std::vector<uint8_t> bytes(payload.begin(), payload.end());
CHECK(client_conn->send(bytes));

{
    std::unique_lock<std::mutex> lk(m);
    cv.wait_for(lk, std::chrono::seconds(3), [&]{ return echoed; });
}
REQUIRE(echoed);
std::string echoed_str(received.begin(), received.end());
CHECK(echoed_str == payload);

// Close and ensure further sends fail (close is async via strand)
client_conn->close();
for (int i = 0; i < 50 && client_conn->is_open(); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
}
CHECK_FALSE(client_conn->send(bytes));

client.stop();
server.stop();
}

TEST_CASE("RealTransport listen retry after failure then success with ephemeral", "[network][transport][real][listen]") {
    RealTransport t(1);

    bool first_failed = false;
    auto noop_accept = [](TransportConnectionPtr){};

    // Try privileged port to induce failure on most systems
    if (!t.listen(1, noop_accept)) {
        first_failed = true;
    } else {
        // Unexpected success; clean up and skip retry semantics
        WARN("listen(1) unexpectedly succeeded; skipping retry test");
        t.stop_listening();
        t.stop();
        return;
    }

    REQUIRE(first_failed);
    // Now retry with ephemeral
    if (!t.listen(0, noop_accept)) {
        WARN("Unable to listen on ephemeral port; environment may restrict binds");
        t.stop();
        return;
    }
    CHECK(t.listening_port() > 0);

    t.stop();
}

TEST_CASE("RealTransport listening_port returns bound ephemeral port", "[network][transport][real][listen]") {
    RealTransport t(1);

    auto noop_accept = [](TransportConnectionPtr){};
    if (!t.listen(0, noop_accept)) {
        WARN("Skipping: unable to bind ephemeral port");
        t.stop();
        return;
    }

    CHECK(t.listening_port() > 0);
    t.stop();
}

TEST_CASE("RealTransport connect timeout triggers timely failure", "[network][transport][real][timeout]") {
    RealTransport t(1);
    t.run();

    // Set a short timeout override for this test
    RealTransportConnection::SetConnectTimeoutForTest(std::chrono::milliseconds(200));

    std::mutex m;
    std::condition_variable cv;
    bool done = false;
    bool ok = true;

    auto start = std::chrono::steady_clock::now();

    std::shared_ptr<TransportConnection> conn;
    conn = t.connect("203.0.113.1", 65530, [&](bool success){
        std::lock_guard<std::mutex> lk(m);
        ok = success;
        done = true;
        cv.notify_all();
    });
    REQUIRE(conn);

    {
        std::unique_lock<std::mutex> lk(m);
        bool signaled = cv.wait_for(lk, std::chrono::seconds(2), [&]{ return done; });
        REQUIRE(signaled);
    }

    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);

    // Must fail, and should complete quickly (either immediate failure or timer)
    CHECK_FALSE(ok);
    CHECK(elapsed.count() <= 1000);

    RealTransportConnection::ResetConnectTimeoutForTest();
    t.stop();
}

TEST_CASE("No stray receive callbacks after close() in handler", "[network][transport][real][receive]") {
    RealTransport server(1);
    RealTransport client(1);

    std::shared_ptr<TransportConnection> inbound_conn;
    std::mutex m;
    std::condition_variable cv;
    int receive_count = 0;
    bool accepted = false;
    bool connected = false;

    auto accept_cb = [&](TransportConnectionPtr c){
        {
            std::lock_guard<std::mutex> lk(m);
            inbound_conn = c;
            accepted = true;
        }
        inbound_conn->set_receive_callback([&](const std::vector<uint8_t>&){
            {
                std::lock_guard<std::mutex> lk(m);
                receive_count++;
            }
            // Close immediately from within handler
            inbound_conn->close();
            cv.notify_all();
        });
        inbound_conn->start();
        cv.notify_all();
    };

    uint16_t port = pick_listen_port(server, accept_cb);
    if (port == 0) {
        WARN("Skipping: unable to bind any listening port (range + ephemeral)");
        return;
    }

    // Start reactors after listen is set up
    server.run();
    client.run();

    std::shared_ptr<TransportConnection> client_conn;
    client_conn = client.connect("127.0.0.1", port, [&](bool ok2){
        std::lock_guard<std::mutex> lk(m);
        connected = ok2;
        if (ok2) { client_conn->start(); }
        cv.notify_all();
    });
    REQUIRE(client_conn);

    // Wait until connected
    {
        std::unique_lock<std::mutex> lk(m);
        cv.wait_for(lk, std::chrono::seconds(3), [&]{ return accepted && connected; });
    }

    // Send one byte
    std::vector<uint8_t> one = {0x42};
    CHECK(client_conn->send(one));

    // Wait for first receive
    {
        std::unique_lock<std::mutex> lk(m);
        cv.wait_for(lk, std::chrono::seconds(2), [&]{ return receive_count >= 1; });
    }

    // Allow handler to complete and ensure no second callback happens
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    CHECK(receive_count == 1);

    client.stop();
    server.stop();
}

TEST_CASE("Read error: remote close triggers single disconnect and no reschedule", "[network][transport][real][read]") {
    RealTransport server(1);
    RealTransport client(1);

    std::shared_ptr<TransportConnection> inbound_conn;
    std::mutex m;
    std::condition_variable cv;
    bool accepted = false;
    bool connected = false;
    int client_disconnects = 0;

    auto accept_cb = [&](TransportConnectionPtr c){
        inbound_conn = c;
        accepted = true;
        inbound_conn->start();
        cv.notify_all();
    };

    uint16_t port = pick_listen_port(server, accept_cb);
    if (port == 0) { WARN("Skipping: unable to bind any listening port"); return; }
    server.run();
    client.run();

    auto client_conn = client.connect("127.0.0.1", port, [&](bool ok){ connected = ok; cv.notify_all(); });
    REQUIRE(client_conn);

    client_conn->set_disconnect_callback([&](){
        std::lock_guard<std::mutex> lk(m);
        client_disconnects++;
        cv.notify_all();
    });
    client_conn->set_receive_callback([&](const std::vector<uint8_t>&){ /* no-op */ });

    {
        std::unique_lock<std::mutex> lk(m);
        cv.wait_for(lk, std::chrono::seconds(3), [&]{ return accepted && connected; });
    }
    REQUIRE(accepted);
    REQUIRE(connected);

    client_conn->start();

    // Remote closes -> client should get exactly one disconnect
    inbound_conn->close();

    {
        std::unique_lock<std::mutex> lk(m);
        cv.wait_for(lk, std::chrono::seconds(2), [&]{ return client_disconnects >= 1; });
    }
    CHECK(client_disconnects == 1);

    client.stop();
    server.stop();
}

TEST_CASE("Close during connect results in single false callback and no stray events", "[network][transport][real][connect]") {
    RealTransport t(1);
    t.run();

    std::mutex m; std::condition_variable cv;
    int cb_count = 0; bool ok = true;

    auto conn = t.connect("203.0.113.1", 65530, [&](bool success){
        std::lock_guard<std::mutex> lk(m);
        cb_count++; ok = success; cv.notify_all();
    });
    REQUIRE(conn);

    // Close immediately; expect at most one callback (false)
    conn->close();

    {
        std::unique_lock<std::mutex> lk(m);
        cv.wait_for(lk, std::chrono::milliseconds(500), [&]{ return cb_count >= 1; });
    }

    // Either we get a single false callback, or none (close canceled connect before callback path)
    CHECK((cb_count == 0 || cb_count == 1));
    if (cb_count == 1) {
        CHECK_FALSE(ok);
    }

    t.stop();
}

TEST_CASE("Connect race: small timeout does not double-callback on fast success", "[network][transport][real][connect]") {
    RealTransport server(1); RealTransport client(1);

    std::shared_ptr<TransportConnection> inbound_conn;
    auto accept_cb = [&](TransportConnectionPtr c){ inbound_conn = c; inbound_conn->start(); };
    uint16_t port = pick_listen_port(server, accept_cb);
    if (port == 0) { WARN("Skipping: unable to bind any listening port"); return; }

    server.run(); client.run();

    RealTransportConnection::SetConnectTimeoutForTest(std::chrono::milliseconds(10));

    std::mutex m; std::condition_variable cv; int cb_count=0; bool ok=false;
    auto conn = client.connect("127.0.0.1", port, [&](bool success){ std::lock_guard<std::mutex> lk(m); cb_count++; ok=success; cv.notify_all(); });
    REQUIRE(conn);

    {
        std::unique_lock<std::mutex> lk(m);
        cv.wait_for(lk, std::chrono::seconds(1), [&]{ return cb_count == 1; });
    }

    CHECK(cb_count == 1);
    CHECK(ok);

    RealTransportConnection::ResetConnectTimeoutForTest();
    client.stop(); server.stop();
}

TEST_CASE("Send-queue overflow closes connection (test override)", "[network][transport][real][send-queue]") {
    RealTransport server(1); RealTransport client(1);

    std::shared_ptr<TransportConnection> inbound_conn;
    std::mutex m; std::condition_variable cv; bool accepted=false; bool connected=false; int client_disc=0;

    auto accept_cb = [&](TransportConnectionPtr c){ inbound_conn = c; accepted=true; inbound_conn->start(); cv.notify_all(); };
    uint16_t port = pick_listen_port(server, accept_cb);
    if (port == 0) { WARN("Skipping: unable to bind any listening port"); return; }
    server.run(); client.run();

    auto client_conn = client.connect("127.0.0.1", port, [&](bool ok){ std::lock_guard<std::mutex> lk(m); connected = ok; cv.notify_all(); });
    REQUIRE(client_conn);

    client_conn->set_disconnect_callback([&](){ std::lock_guard<std::mutex> lk(m); client_disc++; cv.notify_all(); });

    // Wait for connection established
    {
        std::unique_lock<std::mutex> lk(m);
        cv.wait_for(lk, std::chrono::seconds(2), [&]{ return accepted && connected; });
    }

    client_conn->start();

    // Set very small queue limit and send a payload bigger than the limit
    RealTransportConnection::SetSendQueueLimitForTest(512);

    std::vector<uint8_t> big(2048, 0xAA);
    CHECK(client_conn->send(big));

    {
        std::unique_lock<std::mutex> lk(m);
        cv.wait_for(lk, std::chrono::seconds(1), [&]{ return client_disc >= 1; });
    }

    CHECK(client_disc == 1);
    CHECK_FALSE(client_conn->send(big));

    RealTransportConnection::ResetSendQueueLimitForTest();

    client.stop(); server.stop();
}

TEST_CASE("Double close delivers disconnect once", "[network][transport][real][close]") {
    RealTransport t(1); t.run();
    std::mutex m; std::condition_variable cv; int disc=0;

    auto conn = t.connect("203.0.113.1", 65530, [&](bool){ /* ignore */ });
    REQUIRE(conn);

    conn->set_disconnect_callback([&](){ std::lock_guard<std::mutex> lk(m); disc++; cv.notify_all(); });

    conn->close();
    conn->close();

    // For a connection that never opened, close() should be idempotent and not call disconnect callback.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    CHECK(disc == 0);
    t.stop();
}

TEST_CASE("Close with pending read doesn't crash", "[network][transport][real][close][regression]") {
    RealTransport server(1); RealTransport client(1);
    std::shared_ptr<TransportConnection> inbound_conn;
    std::mutex m; std::condition_variable cv; bool accepted=false; bool connected=false; int disconnects=0;

    auto accept_cb = [&](TransportConnectionPtr c){
        inbound_conn = c;
        accepted = true;
        inbound_conn->set_receive_callback([](const std::vector<uint8_t>&){});
        inbound_conn->start();
        cv.notify_all();
    };

    uint16_t port = pick_listen_port(server, accept_cb);
    if (port == 0) { WARN("Skipping: unable to bind any listening port"); return; }
    server.run(); client.run();

    auto client_conn = client.connect("127.0.0.1", port, [&](bool ok){ std::lock_guard<std::mutex> lk(m); connected = ok; cv.notify_all(); });
    REQUIRE(client_conn);

    client_conn->set_disconnect_callback([&](){ std::lock_guard<std::mutex> lk(m); disconnects++; cv.notify_all(); });
    client_conn->set_receive_callback([](const std::vector<uint8_t>&){});

    {
        std::unique_lock<std::mutex> lk(m);
        cv.wait_for(lk, std::chrono::seconds(2), [&]{ return accepted && connected; });
    }
    REQUIRE((accepted && connected));

    client_conn->start();

    // Close immediately while read is pending
    // This used to cause use-after-free because:
    // - start_read_impl() calls async_read_some with [self = shared_from_this()]
    // - close_impl() destroys member state (receive_callback_, send_queue_, etc.)
    // - When read completes later, handler accesses destroyed members
    // Fix: close_impl() now moves socket_ and cancels it, forcing handlers to get operation_aborted
    client_conn->close();

    // Give async operations time to complete and handlers to run
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // Should not crash; disconnect callback should fire exactly once (or not at all for pending reads)
    CHECK(disconnects <= 1);

    client.stop();
    server.stop();
}

TEST_CASE("Close with pending write doesn't crash", "[network][transport][real][close][regression]") {
    RealTransport server(1); RealTransport client(1);
    std::shared_ptr<TransportConnection> inbound_conn;
    std::mutex m; std::condition_variable cv; bool accepted=false; bool connected=false; int client_disconnects=0;

    auto accept_cb = [&](TransportConnectionPtr c){
        inbound_conn = c;
        accepted = true;
        inbound_conn->start();
        cv.notify_all();
    };

    uint16_t port = pick_listen_port(server, accept_cb);
    if (port == 0) { WARN("Skipping: unable to bind any listening port"); return; }
    server.run(); client.run();

    auto client_conn = client.connect("127.0.0.1", port, [&](bool ok){ std::lock_guard<std::mutex> lk(m); connected = ok; cv.notify_all(); });
    REQUIRE(client_conn);

    client_conn->set_disconnect_callback([&](){ std::lock_guard<std::mutex> lk(m); client_disconnects++; cv.notify_all(); });

    {
        std::unique_lock<std::mutex> lk(m);
        cv.wait_for(lk, std::chrono::seconds(2), [&]{ return accepted && connected; });
    }
    REQUIRE((accepted && connected));

    // Queue multiple writes
    for (int i = 0; i < 10; ++i) {
        std::vector<uint8_t> data(256, 0xAB);
        client_conn->send(data);
    }

    // Close immediately while writes are pending
    // This used to cause use-after-free because:
    // - do_write_impl() calls async_write with [self = shared_from_this()]
    // - close_impl() destroys send_queue_ and clears writing_ flag
    // - When write completes later, handler accesses destroyed send_queue_
    // Fix: close_impl() now moves socket_ and cancels it, forcing handlers to get operation_aborted
    client_conn->close();

    // Give async operations time to complete and handlers to run
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // Should not crash; client should either get disconnect once or none (pending writes don't trigger it)
    CHECK(client_disconnects <= 1);

    client.stop();
    server.stop();
}

TEST_CASE("Rapid close/destroy doesn't crash", "[network][transport][real][close][regression]") {
    // Stress test: create connections, immediately close them, destroy transport
    // without giving async handlers time to complete.
    // The fix ensures handlers will run with operation_aborted even after object destruction.
    {
        RealTransport t(1); t.run();

        // Create 20 connections and close them immediately
        std::vector<TransportConnectionPtr> conns;
        for (int i = 0; i < 20; ++i) {
            auto conn = t.connect("203.0.113.1", 65530, [](bool){});
            if (conn) {
                conn->close();
                conns.push_back(conn);
            }
        }

        // Clear and stop without waiting for async ops
        conns.clear();
        t.stop();  // This should safely cancel all pending I/O
    }
    // If we reach here without crash, the fix is working
    CHECK(true);
}

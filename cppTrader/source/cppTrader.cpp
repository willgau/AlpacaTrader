#include "secrets_local.h"
#include "common.h"
#include "myboost.h"
#include "Porfolio.h"

#include "FastQueue.hpp"

static nlohmann::json parse_ws_payload(const beast::flat_buffer& buf, bool is_binary) {
    const auto* data = static_cast<const std::uint8_t*>(buf.data().data());
    const std::size_t n = buf.data().size();

    if (!is_binary) {
        return nlohmann::json::parse(
            std::string_view(reinterpret_cast<const char*>(data), n)
        );
    }

    // Binary frames: try MessagePack first; fall back to JSON bytes.
    try {
        std::vector<std::uint8_t> v(data, data + n);
        return nlohmann::json::from_msgpack(v);
    }
    catch (...) {
        return nlohmann::json::parse(
            std::string_view(reinterpret_cast<const char*>(data), n)
        );
    }
}

// Helper: async sleep/backoff
static awaitable<void> async_sleep(std::chrono::milliseconds d) {
    auto ex = co_await asio::this_coro::executor;
    asio::steady_timer t(ex);
    t.expires_after(d);
    co_await t.async_wait(use_awaitable);
}

// Optional: route decoded events somewhere real.
static void handle_event(const nlohmann::json& msg) {
    // In production you’d normalize + enqueue into a bounded SPSC ring.
    // For demo:
    std::cout << msg.dump() << "\n";
}

// One full connect->auth->listen->read session.
// Returns only on error/disconnect (caller handles reconnect).
static awaitable<void> run_one_session(
    std::string host,
    std::string port,
    std::string path,
    std::string key_id,
    std::string secret,
    ssl::context& tls_ctx
) {
    auto ex = co_await asio::this_coro::executor;

    // 1) Resolve
    tcp::resolver resolver(ex);
    beast::error_code ec;

    auto results = co_await resolver.async_resolve(host, port,
        asio::redirect_error(use_awaitable, ec));
    if (ec) {
        throw beast::system_error(ec, "resolve");
    }

    // 2) TCP connect + TLS stream
    beast::ssl_stream<beast::tcp_stream> tls_stream(ex, tls_ctx);

    // SNI (important with modern TLS hosting)
    if (!SSL_set_tlsext_host_name(tls_stream.native_handle(), host.c_str())) {
        beast::error_code sni_ec(
            static_cast<int>(::ERR_get_error()),
            asio::error::get_ssl_category()
        );
        throw beast::system_error(sni_ec, "SNI");
    }

    // Connect lowest layer (TCP)
    co_await beast::get_lowest_layer(tls_stream).async_connect(
        results, asio::redirect_error(use_awaitable, ec));
    if (ec) {
        throw beast::system_error(ec, "connect");
    }

    // 3) TLS handshake
    co_await tls_stream.async_handshake(ssl::stream_base::client,
        asio::redirect_error(use_awaitable, ec));
    if (ec) {
        throw beast::system_error(ec, "tls_handshake");
    }

    // 4) WebSocket over TLS
    ws::stream<beast::ssl_stream<beast::tcp_stream>> sock(std::move(tls_stream));

    // Reasonable timeouts; tune for your needs.
    sock.set_option(ws::stream_base::timeout::suggested(beast::role_type::client));
    sock.set_option(ws::stream_base::decorator(
        [](ws::request_type& req) {
            req.set(beast::http::field::user_agent, "alpaca-ws-async/1.0");
        }
    ));

    // WebSocket handshake
    co_await sock.async_handshake(host, path,
        asio::redirect_error(use_awaitable, ec));
    if (ec) {
        throw beast::system_error(ec, "ws_handshake");
    }

    // We will be sending JSON text.
    sock.text(true);

    // 5) Auth
    {
        nlohmann::json auth = {
            {"action", "auth"},
            {"key", key_id},
            {"secret", secret}
        };
        const std::string payload = auth.dump();
        co_await sock.async_write(asio::buffer(payload),
            asio::redirect_error(use_awaitable, ec));
        if (ec) {
            throw beast::system_error(ec, "write_auth");
        }
    }

    // 6) Listen to trade_updates
    {
        nlohmann::json listen = {
            {"action", "listen"},
            {"data", {{"streams", {"trade_updates"}}}}
        };
        const std::string payload = listen.dump();
        co_await sock.async_write(asio::buffer(payload),
            asio::redirect_error(use_awaitable, ec));
        if (ec) {
            throw beast::system_error(ec, "write_listen");
        }
    }

    // 7) Read loop
    for (;;) {
        beast::flat_buffer buf;
        co_await sock.async_read(buf, asio::redirect_error(use_awaitable, ec));
        if (ec) {
            // Normal disconnects often come here.
            throw beast::system_error(ec, "read");
        }

        const bool is_binary = sock.got_binary();
        nlohmann::json msg;

        try {
            msg = parse_ws_payload(buf, is_binary);
        }
        catch (const std::exception& e) {
            // Parsing failure: in production, log raw bytes and decide policy
            std::cerr << "parse error: " << e.what() << "\n";
            continue;
        }

        // Optional: you can detect auth/subscription acks here and gate readiness.
        handle_event(msg);
    }
}

// Reconnect supervisor with exponential backoff.
static awaitable<void> run_forever() 
{
    auto ex = co_await asio::this_coro::executor;

    // Configure here:
    const std::string host = "paper-api.alpaca.markets"; // or api.alpaca.markets
    const std::string port = "443";
    const std::string path = "/stream";
    const std::string key_id = APCA_KEY_ID;
    const std::string secret = APCA_SECRET;

    // TLS context (shared across reconnect attempts)
    ssl::context tls_ctx(ssl::context::tls_client);

    // Use system roots; for strict setups you may load pinned roots instead.
    tls_ctx.set_default_verify_paths();
    tls_ctx.set_verify_mode(ssl::verify_peer);

    // Load root CAs (PEM bundle)
    tls_ctx.load_verify_file(CACERT_LOCATION);

    // Hostname verification (Boost helper)
    tls_ctx.set_verify_callback(ssl::host_name_verification(host));

    std::chrono::milliseconds backoff{ 250 };
    const std::chrono::milliseconds backoff_max{ 10'000 };

    while(true) {
        try 
        {
            co_await run_one_session(host, port, path, key_id, secret, tls_ctx);
        }
        catch (const std::exception& e) 
        {
            std::cerr << "[session error] " << e.what() << "\n";
        }

        // Backoff + jitter (very basic)
        const int jitter_ms = std::rand() % 200; // 0..199
        co_await async_sleep(backoff + std::chrono::milliseconds(jitter_ms));

        // Exponential backoff with cap
        backoff = std::min(backoff * 2, backoff_max);
    }
}
//temp function
void worker()
{
    using Q = FastQueue<(1u << 20), 8, (1u << 16)>;

    Q q;
    auto prod = q.make_producer();
    auto cons = q.make_consumer();

    const char msg[] = "hello";
    prod.write(std::as_bytes(std::span{ msg, sizeof(msg) }));

    std::array<std::byte, 64> buf{};
    int n = cons.try_read(buf);
    if (n > 0) {
        std::cout << "read " << n << " bytes: "
            << reinterpret_cast<const char*>(buf.data()) << "\n";
    }
}

int main() {

    boost::thread t(&worker);

    try 
    {
        asio::io_context ioc;

        asio::co_spawn(ioc, run_forever(), asio::detached);
        asio::co_spawn(ioc, Portfolio::getInstance("Will", 900).poll_account_forever(), asio::detached);
        
        

        ioc.run();

    }
    catch (const std::exception& e) {
        std::cerr << "fatal: " << e.what() << "\n";
        return 1;
    }

    t.join();

    return 0;
}

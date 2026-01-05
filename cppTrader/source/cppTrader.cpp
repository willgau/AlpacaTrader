#include "secrets_local.h"
#include "common.h"
#include "myboost.h"
#include "Porfolio.h"

#include "FastQueue.hpp"
#include "Benchmark.h"
#include "OrderType.h"

// Helper: parse WebSocket payload (text or binary) into JSON.
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


template <class Consumer>
awaitable<void> consumer_run_forever( Consumer consumer, BenchConfig benchmark, std::uint64_t qpcFrequency, boost::barrier& start_barrier, std::atomic<bool>& producer_done, BenchResults& out) 
{
    pin_current_thread_to_cpu(1);

    auto ex = co_await asio::this_coro::executor;
    asio::steady_timer t(ex);

    std::array<std::byte, 256> buf{};

    long double sum_ns_128 = 0;

    std::uint32_t sample_counter = 0;

    start_barrier.wait();

    while (out.consumed < benchmark.messages) 
    {
        int n = consumer.try_read(buf);
        if (n <= 0) {
            // async backoff (does not block io_context)
            t.expires_after(benchmark.empty_backoff);
            co_await t.async_wait(use_awaitable);
            continue;
        }

        if ((std::size_t)n != sizeof(OrderMsg)) {
            // unexpected; skip
            continue;
        }

        OrderMsg m{};
        std::memcpy(&m, buf.data(), sizeof(m));

        // "process": update checksum so compiler can’t erase the loop
        out.checksum += (m.seq * 1315423911ull) ^ (m.qty * 2654435761ull);

        // latency sampling
        if (++sample_counter >= benchmark.sample_every) 
        {
            sample_counter = 0;

            std::uint64_t now = qpc_now();
            std::uint64_t dt_ticks = (now >= m.ts_qpc) ? (now - m.ts_qpc) : 0;
            std::uint64_t dt_ns = ticks_to_ns(dt_ticks, qpcFrequency);

            if (dt_ns < out.min_ns) out.min_ns = dt_ns;
            if (dt_ns > out.max_ns) out.max_ns = dt_ns;

            out.hist.add(dt_ns);

            sum_ns_128 += (long double)dt_ns;
        }

        ++out.consumed;

        (void)producer_done.load(std::memory_order_acquire);
    }

    if (out.hist.total > 0) 
    {
        out.avg_ns = (double)((unsigned long long)(sum_ns_128 / out.hist.total));
    }
    else 
    {
        out.min_ns = 0;
        out.avg_ns = 0.0;
        out.max_ns = 0;
    }

    co_return;
}

//template <class Consumer>
//void consumer_thread_fn( Consumer consumer, BenchConfig benchmark, boost::barrier& start_barrier, std::atomic<bool>& producer_done,  std::uint64_t& out_consumed,  std::uint64_t& out_checksum) 
//{
//    pin_current_thread_to_cpu(1);
//
//    std::array<std::byte, 4096> buf{};
//
//    if (benchmark.payload_bytes > buf.size()) {
//        throw std::runtime_error("payload_bytes too large for consumer buffer");
//    }
//
//    out_consumed = 0;
//    out_checksum = 0;
//
//    // Wait until producer is ready
//    start_barrier.wait();
//
//    while (out_consumed < benchmark.messages) 
//    {
//        int n = consumer.try_read(std::span<std::byte>(buf.data(), buf.size()));
//        if (n > 0) 
//        {
//            // Basic correctness: check size matches what producer wrote
//            if ((std::size_t)n < sizeof(std::uint64_t)) 
//            {
//                throw std::runtime_error("message too small");
//            }
//
//            // Optional "work": read sequence and update checksum
//            if (benchmark.do_consumer_work) 
//            {
//                std::uint64_t seq = 0;
//                std::memcpy(&seq, buf.data(), sizeof(seq));
//                out_checksum += (seq * 1315423911ull) ^ (seq >> 7);
//            }
//
//            ++out_consumed;
//        }
//        else 
//        {
//            // empty: mild backoff (choose ONE style)
//            boost::this_thread::yield();
//        }
//
//        // If producer is done but we haven't consumed all messages, keep draining.
//        // This is just a safety; the loop condition is out_consumed < messages.
//        (void)producer_done.load(std::memory_order_acquire);
//    }
//}

template <class Producer>
void producer_thread_fn( Producer producer, BenchConfig cfg, boost::barrier& start_barrier, std::atomic<bool>& producer_done) {
    pin_current_thread_to_cpu(0);

    start_barrier.wait();

    for (std::uint64_t i = 0; i < cfg.messages; ++i) {
        // Alternate Buy/Sell deterministically
        OrderMsg m = make_msg(i, (i & 1) == 0);
        
        //to debug
        //print_msg(m);
        producer.write(std::as_bytes(std::span{ &m, 1 }));
    }

    producer_done.store(true, std::memory_order_release);
}

int main() 
{
    try
    {
        const std::uint64_t freq = qpc_freq();

        BenchConfig benchmark;
        BenchResults results;

        benchmark.messages = 5'000'000;
        benchmark.sample_every = 1;
        benchmark.empty_backoff = std::chrono::microseconds(10);

        using Q = FastQueue<(1u << 20), 8, (1u << 16)>;

        Q q;
        auto prod = q.make_producer();
        auto cons = q.make_consumer();

        // Barrier to start both threads at the same time
        boost::barrier start_barrier(2);
        std::atomic<bool> producer_done{ false };

        // Start the asio event loop in this main thread
        asio::io_context ioc;
         
        // Start timepoints
        //auto start_time = std::chrono::steady_clock::time_point{};
        //auto end_time = std::chrono::steady_clock::time_point{};
        auto timing = std::make_shared<TimingState>();

        auto consumer = std::move(cons);    // move consumer out before co_spawn
        auto bench = benchmark;             // copy config (small)
        auto freq_l = freq;                 // copy

        // Launch io_context in its own OS thread
        boost::thread io_thread([&] 
            {
                pin_current_thread_to_cpu(2);
                ioc.run();
            });

        // Spawn consumer coroutine
        asio::co_spawn(
            ioc,
            [consumer = std::move(consumer),
            bench,
            freq_l,
            &start_barrier,
            &producer_done,
            &results,
            timing,
            &ioc]() mutable -> awaitable<void>
            {
                timing->start = std::chrono::steady_clock::now();

                co_await consumer_run_forever(
                    std::move(consumer),
                    bench,
                    freq_l,
                    start_barrier,
                    producer_done,
                    results
                );

                timing->end = std::chrono::steady_clock::now();
                ioc.stop();
                co_return;
            },
            asio::detached
        );

        // Producer thread
        boost::thread producer_thr( &producer_thread_fn<decltype(prod)>, std::move(prod), benchmark, std::ref(start_barrier), std::ref(producer_done));

        producer_thr.join();
        io_thread.join();

        const double seconds = std::chrono::duration_cast<std::chrono::duration<double>>(timing->end - timing->start).count();

        const double msg_per_sec = double(benchmark.messages) / seconds;
        const double bytes_per_sec = (double(benchmark.messages) * double(sizeof(OrderMsg))) / seconds;
        const double mib_per_sec = bytes_per_sec / (1024.0 * 1024.0);

        std::cout << "Messages   : " << benchmark.messages << "\n";
        std::cout << "Msg size   : " << sizeof(OrderMsg) << " bytes\n";
        std::cout << "Time       : " << seconds << " s\n";
        std::cout << "Throughput : " << msg_per_sec << " msg/s\n";
        std::cout << "Bandwidth  : " << mib_per_sec << " MiB/s\n";
        std::cout << "Consumed   : " << results.consumed << "\n";
        std::cout << "Checksum   : " << results.checksum << "\n";

        if (results.hist.total > 0) 
        {
            std::cout << "\nLatency (ns) over " << results.hist.total << " samples:\n";
            std::cout << "  min   : " << results.min_ns << "\n";
            std::cout << "  p50~  : " << results.hist.percentile(0.50) << " (bucket upper bound)\n";
            std::cout << "  p99~  : " << results.hist.percentile(0.99) << " (bucket upper bound)\n";
            std::cout << "  p99.9~: " << results.hist.percentile(0.999) << " (bucket upper bound)\n";
            std::cout << "  max   : " << results.max_ns << "\n";
            std::cout << "  avg   : " << results.avg_ns << "\n";
        }

    }
    catch (const std::exception& e) {
        std::cerr << "fatal: " << e.what() << "\n";
        return 1;
    }

    return 0;
}

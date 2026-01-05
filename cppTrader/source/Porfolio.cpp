
#include "Porfolio.h"


Portfolio::Portfolio(const std::string& iName, const double& iCash, const std::string& iHost, const std::string& iPort) 
    : mName(iName), mCash(iCash), mHost(iHost), mPort(iPort)

{
    std::cout << "Creating account Portfolio: " << iName << " with cash: " << iCash << "\n" << std::endl;

}



awaitable<nlohmann::json> Portfolio::alpaca_get_account( ssl::context& tls_ctx) {
    auto ex = co_await asio::this_coro::executor;
    beast::error_code ec;

    // Resolve
    tcp::resolver resolver(ex);
    auto results = co_await resolver.async_resolve( mHost, mPort, asio::redirect_error(use_awaitable, ec));
    if (ec) throw beast::system_error(ec, "resolve");

    // TCP + TLS
    beast::ssl_stream<beast::tcp_stream> tls_stream(ex, tls_ctx);

    // SNI
    if (!SSL_set_tlsext_host_name(tls_stream.native_handle(), mHost.c_str())) {
        beast::error_code sni_ec(
            static_cast<int>(::ERR_get_error()),
            asio::error::get_ssl_category()
        );
        throw beast::system_error(sni_ec, "SNI");
    }

    co_await beast::get_lowest_layer(tls_stream).async_connect(
        results, asio::redirect_error(use_awaitable, ec));
    if (ec) throw beast::system_error(ec, "connect");

    co_await tls_stream.async_handshake(
        ssl::stream_base::client, asio::redirect_error(use_awaitable, ec));
    if (ec) throw beast::system_error(ec, "tls_handshake");

    // Build HTTP request: GET /v2/account
    http::request<http::string_body> req{ http::verb::get, "/v2/account", 11 };
    req.set(http::field::host, mHost);
    req.set(http::field::user_agent, "alpaca-rest-async/1.0");
    req.set(http::field::accept, "application/json");
    req.set("APCA-API-KEY-ID", APCA_KEY_ID);
    req.set("APCA-API-SECRET-KEY", APCA_SECRET);

    // Send request
    co_await http::async_write(tls_stream, req, asio::redirect_error(use_awaitable, ec));
    if (ec) throw beast::system_error(ec, "http_write");

    // Read response
    beast::flat_buffer buffer;
    http::response<http::string_body> res;
    co_await http::async_read(tls_stream, buffer, res, asio::redirect_error(use_awaitable, ec));
    if (ec) throw beast::system_error(ec, "http_read");

    // Graceful TLS shutdown (ignore EOF which is common on shutdown)
    co_await tls_stream.async_shutdown(asio::redirect_error(use_awaitable, ec));
    if (ec == asio::error::eof) ec = {};
    // (Optional) if ec still set, log it; not always fatal.

    if (res.result() != http::status::ok) {
        throw std::runtime_error("GET /v2/account failed: HTTP " + std::to_string(res.result_int())
            + " body=" + res.body());
    }

    co_return nlohmann::json::parse(res.body());
}

 asio::awaitable<void> Portfolio::poll_account_forever() {
     
    ssl::context tls_ctx(ssl::context::tls_client);
    tls_ctx.set_default_verify_paths();
    tls_ctx.set_verify_mode(ssl::verify_peer);

    // Load root CAs (PEM bundle)
    tls_ctx.load_verify_file(CACERT_LOCATION);

    tls_ctx.set_verify_callback(ssl::host_name_verification(mHost));

    auto ex = co_await asio::this_coro::executor;
    asio::steady_timer t(ex);

    while(true) 
    {
        try 
        {
            auto account = co_await alpaca_get_account(tls_ctx);

            const std::string equity = account.value("equity", "0");
            const std::string cash = account.value("cash", "0");
            const std::string bp = account.value("buying_power", "0");

            std::cout << "equity=" << equity
                << " cash=" << cash
                << " buying_power=" << bp << "\n";

            mCash = std::stod(cash);
        }
        catch (const std::exception& e) {
            std::cerr << "[account poll error] " << e.what() << "\n";
        }

        t.expires_after(std::chrono::seconds(10));
        co_await t.async_wait(use_awaitable);
    }
}

 awaitable<nlohmann::json> Portfolio::alpaca_post_order( const nlohmann::json& order, ssl::context& tls_ctx)
 {
     auto ex = co_await asio::this_coro::executor;
     beast::error_code ec;

     // Resolve
     tcp::resolver resolver(ex);
     auto results = co_await resolver.async_resolve( mHost, mPort, asio::redirect_error(use_awaitable, ec));
     if (ec) throw beast::system_error(ec, "resolve");

     // TCP + TLS
     beast::ssl_stream<beast::tcp_stream> tls_stream(ex, tls_ctx);

     // SNI (important)
     if (!SSL_set_tlsext_host_name(tls_stream.native_handle(), mHost.c_str())) {
         beast::error_code sni_ec(
             static_cast<int>(::ERR_get_error()),
             asio::error::get_ssl_category()
         );
         throw beast::system_error(sni_ec, "SNI");
     }

     co_await beast::get_lowest_layer(tls_stream).async_connect(
         results, asio::redirect_error(use_awaitable, ec));
     if (ec) throw beast::system_error(ec, "connect");

     co_await tls_stream.async_handshake(
         ssl::stream_base::client, asio::redirect_error(use_awaitable, ec));
     if (ec) throw beast::system_error(ec, "tls_handshake");

     // Build HTTP request: POST /v2/orders
     http::request<http::string_body> req{ http::verb::post, "/v2/orders", 11 };
     req.set(http::field::host, mHost);
     req.set(http::field::user_agent, "alpaca-rest-async/1.0");
     req.set(http::field::accept, "application/json");
     req.set(http::field::content_type, "application/json");

     // Alpaca auth headers
     req.set("APCA-API-KEY-ID", APCA_KEY_ID);
     req.set("APCA-API-SECRET-KEY", APCA_SECRET);

     req.body() = order.dump();
     req.prepare_payload();

     // Send request
     co_await http::async_write(tls_stream, req, asio::redirect_error(use_awaitable, ec));
     if (ec) throw beast::system_error(ec, "http_write");

     // Read response
     beast::flat_buffer buffer;
     http::response<http::string_body> res;

     co_await http::async_read(tls_stream, buffer, res, asio::redirect_error(use_awaitable, ec));

     if (ec) throw beast::system_error(ec, "http_read");

     // Shutdown TLS (EOF is common here)
     co_await tls_stream.async_shutdown(asio::redirect_error(use_awaitable, ec));
     if (ec == asio::error::eof) ec = {};

     if (res.result() != http::status::ok) {
         throw std::runtime_error(
             "POST /v2/orders failed: HTTP " + std::to_string(res.result_int()) +
             " body=" + res.body()
         );
     }

     co_return nlohmann::json::parse(res.body());
 }
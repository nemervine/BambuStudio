#include "ControlServer.hpp"
#include "ControlHandlers.hpp"

#include <fstream>
#include <random>

#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>

#include "libslic3r/Utils.hpp"     // data_dir()

namespace Slic3r { namespace GUI { namespace ControlAPI {

namespace http = boost::beast::http;

static std::string generate_token()
{
    // 32 hex chars from a hardware-seeded PRNG. This is an access token for a
    // localhost-only, same-user API — not a cryptographic secret shared over a
    // network — so std::random_device seeding is sufficient.
    static const char* hex = "0123456789abcdef";
    std::random_device rd;
    std::mt19937_64    gen(((std::uint64_t) rd() << 32) ^ rd());
    std::uniform_int_distribution<int> dist(0, 15);
    std::string token;
    token.reserve(32);
    for (int i = 0; i < 32; ++i)
        token.push_back(hex[dist(gen)]);
    return token;
}

ControlServer::ControlServer() = default;

ControlServer::~ControlServer() { stop(); }

bool ControlServer::start(int port)
{
    if (running_.exchange(true))
        return true;

    port_  = port;
    token_ = generate_token();

    // Write the token file next to the app config so a local client (the MCP
    // server) can authenticate. Refuse to start the server if this fails —
    // running without a readable token would make the API unusable anyway.
    const boost::filesystem::path token_path = boost::filesystem::path(data_dir()) / "control_api_token";
    {
        std::ofstream out(token_path.string(), std::ios::trunc);
        out << token_;
        if (!out.good()) {
            BOOST_LOG_TRIVIAL(error) << "control_api: failed to write token file " << token_path;
            running_ = false;
            return false;
        }
    }

    using tcp = boost::asio::ip::tcp;
    guard_    = std::make_unique<boost::asio::executor_work_guard<decltype(io_.get_executor())>>(io_.get_executor());

    // 127.0.0.1 only — never expose this API beyond the local machine.
    tcp::endpoint             ep{boost::asio::ip::make_address_v4("127.0.0.1"), (unsigned short) port_};
    boost::system::error_code ec;
    acceptor_.open(ep.protocol(), ec);
    if (!ec) acceptor_.set_option(tcp::acceptor::reuse_address(true), ec);
    if (!ec) acceptor_.bind(ep, ec);
    if (!ec) acceptor_.listen(boost::asio::socket_base::max_listen_connections, ec);
    if (ec) {
        BOOST_LOG_TRIVIAL(error) << "control_api: failed to listen on 127.0.0.1:" << port_ << ": " << ec.message();
        guard_.reset();
        running_ = false;
        return false;
    }

    do_accept();
    worker_ = boost::thread([this] { io_.run(); });
    BOOST_LOG_TRIVIAL(info) << "control_api: listening on 127.0.0.1:" << port_ << ", token file " << token_path;
    return true;
}

void ControlServer::stop()
{
    if (!running_.exchange(false))
        return;
    boost::system::error_code ec;
    acceptor_.close(ec);
    io_.stop();
    guard_.reset();
    if (worker_.joinable())
        worker_.join();
    io_.restart();
}

void ControlServer::do_accept()
{
    acceptor_.async_accept([this](boost::system::error_code ec, boost::asio::ip::tcp::socket socket) {
        if (!ec && running_)
            std::make_shared<ControlSession>(std::move(socket), *this)->run();
        if (running_)
            do_accept();
    });
}

// --------------------------------------------------------------------------
// ControlSession
// --------------------------------------------------------------------------

void ControlSession::do_read()
{
    auto self = shared_from_this();
    req_      = {};
    http::async_read(socket_, buffer_, req_, [self](boost::beast::error_code ec, std::size_t) {
        if (ec) {
            boost::beast::error_code ignored;
            self->socket_.shutdown(boost::asio::ip::tcp::socket::shutdown_both, ignored);
            return;
        }
        self->handle_request();
    });
}

void ControlSession::handle_request()
{
    // Uniform envelope: {"ok": true, "data": ...} / {"ok": false, "error": {...}}
    int  http_status = 200;
    json envelope;
    try {
        // Authentication first: constant behavior for every route.
        auto auth = req_.find(http::field::authorization);
        const std::string expected = "Bearer " + server_.token();
        if (auth == req_.end() || std::string(auth->value()) != expected)
            throw ApiError(401, "unauthorized", "missing or invalid bearer token (see <datadir>/control_api_token)");

        const std::string target(req_.target());
        const size_t      qpos  = target.find('?');
        const std::string path  = qpos == std::string::npos ? target : target.substr(0, qpos);
        const std::string query = qpos == std::string::npos ? std::string() : target.substr(qpos + 1);

        json data       = dispatch(std::string(req_.method_string()), path, query, req_.body(), http_status);
        envelope["ok"]   = true;
        envelope["data"] = std::move(data);
    } catch (const ApiError& e) {
        http_status                  = e.http_status();
        envelope["ok"]               = false;
        envelope["error"]["code"]    = e.code();
        envelope["error"]["message"] = e.what();
    } catch (const std::exception& e) {
        http_status                  = 500;
        envelope["ok"]               = false;
        envelope["error"]["code"]    = "internal_error";
        envelope["error"]["message"] = e.what();
    }

    write_response(http_status, envelope.dump());
}

void ControlSession::write_response(int http_status, const std::string& json_body)
{
    auto res = std::make_shared<http::response<http::string_body>>(http::int_to_status(http_status), 11);
    res->set(http::field::content_type, "application/json; charset=utf-8");
    res->keep_alive(false);
    res->body() = json_body;
    res->prepare_payload();

    auto self = shared_from_this();
    http::async_write(socket_, *res, [self, res](boost::beast::error_code ec, std::size_t) {
        boost::beast::error_code ignored;
        self->socket_.shutdown(boost::asio::ip::tcp::socket::shutdown_send, ignored);
    });
}

}}} // namespace Slic3r::GUI::ControlAPI

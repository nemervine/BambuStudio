#ifndef slic3r_ControlAPI_ControlServer_hpp_
#define slic3r_ControlAPI_ControlServer_hpp_

// Local Control API: a localhost-only HTTP+JSON server embedded in the GUI so
// external tools (e.g. an MCP server driving Bambu Studio for an AI assistant)
// can automate the running instance: open projects, change presets, transform
// objects, slice, and read results.
//
// Design notes:
// - Network layer modeled on GUI/HttpServer.{hpp,cpp} (Boost.Beast acceptor +
//   per-connection session on a dedicated io_context thread).
// - The network thread NEVER touches wxWidgets state. Handlers marshal work to
//   the UI thread via wxTheApp->CallAfter and wait on a std::future with a
//   timeout (see ControlHandlers.cpp run_on_ui()).
// - Bound to 127.0.0.1 only. Every request must carry the bearer token that is
//   written to <datadir>/control_api_token at startup (file permissions are the
//   trust boundary — same user only).
// - Disabled by default; enabled by app config key "enable_control_api" or the
//   BAMBU_CONTROL_API=1 environment variable (see GUI_App::post_init()).

#include <atomic>
#include <memory>
#include <string>

#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/thread.hpp>

namespace Slic3r { namespace GUI { namespace ControlAPI {

constexpr int DEFAULT_CONTROL_API_PORT = 13639;

class ControlServer
{
public:
    ControlServer();
    ~ControlServer();

    bool is_started() const { return running_.load(); }

    // Generates the session token, writes the token file, binds 127.0.0.1:port
    // and starts the worker thread. Returns false (with a log message) if the
    // port is unavailable or the token file cannot be written.
    bool start(int port = DEFAULT_CONTROL_API_PORT);
    void stop();

    int                port() const { return port_; }
    const std::string& token() const { return token_; }

private:
    void do_accept();

    std::atomic_bool                                                                          running_{false};
    int                                                                                       port_{DEFAULT_CONTROL_API_PORT};
    std::string                                                                               token_;
    boost::asio::io_context                                                                   io_{1};
    std::unique_ptr<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>> guard_;
    boost::asio::ip::tcp::acceptor                                                            acceptor_{io_};
    boost::thread                                                                             worker_;
};

// One HTTP connection. Reads a request, routes it through ControlHandlers,
// writes the JSON response, then closes (Connection: close semantics keep the
// session lifecycle trivial; clients are local so reconnect cost is nil).
struct ControlSession : public std::enable_shared_from_this<ControlSession>
{
    ControlSession(boost::asio::ip::tcp::socket socket, const ControlServer& server)
        : socket_(std::move(socket)), server_(server)
    {}

    void run() { do_read(); }

private:
    void do_read();
    void handle_request();
    void write_response(int http_status, const std::string& json_body);

    boost::asio::ip::tcp::socket                             socket_;
    const ControlServer&                                     server_;
    boost::beast::flat_buffer                                buffer_;
    boost::beast::http::request<boost::beast::http::string_body> req_;
};

}}} // namespace Slic3r::GUI::ControlAPI

#endif // slic3r_ControlAPI_ControlServer_hpp_

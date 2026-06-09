#pragma once

#include <memory>

#include <asio.hpp>
#include <restinio/core.hpp>
#include <restinio/router/express.hpp>

class ProcessManager;
class PipeServer;

class HttpServer {
public:
    HttpServer(asio::io_context& io, ProcessManager& pm, PipeServer& ps, int port = 9580);
    ~HttpServer();
    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;

    void Open();
    void Close();

    int Port() const { return port_; }

private:
    using router_t = restinio::router::express_router_t<>;
    using traits_t = restinio::traits_t<restinio::asio_timer_manager_t, restinio::null_logger_t, router_t>;
    using server_t = restinio::http_server_t<traits_t>;

    void RegisterRoutes(router_t& router);

    asio::io_context&           io_;
    ProcessManager&             pm_;
    PipeServer&                 ps_;
    int                         port_;
    std::unique_ptr<server_t>   server_;
};

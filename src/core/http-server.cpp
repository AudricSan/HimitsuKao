#include "http-server.h"

#include <chrono>
#include <exception>

#include <httplib.h>

namespace himitsukao {

HttpServer::HttpServer(ApiHandler &api) : api_(api) {}

HttpServer::~HttpServer()
{
	stop();
}

bool HttpServer::start(const std::string &host, int port, std::string *error)
{
	if (running_)
		return true;

	auto server = std::make_unique<httplib::Server>();
	server->set_payload_max_length(kMaxBodyBytes);
	server->set_read_timeout(5, 0);
	server->set_write_timeout(5, 0);
	server->set_keep_alive_max_count(20);
	// httplib's defaults would let a second process bind the same port on Windows (SO_REUSEADDR) or on Linux
	// (SO_REUSEPORT): use exclusive binding so that a busy port is detected and cannot be hijacked.
	server->set_socket_options([](socket_t sock) {
		int opt = 1;
#ifdef _WIN32
		setsockopt(sock, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char *>(&opt), sizeof(opt));
#else
		setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const void *>(&opt), sizeof(opt));
#endif
	});
	server->new_task_queue = [] { return new httplib::ThreadPool(4); };

	ApiHandler *api = &api_;
	auto handler = [api](const httplib::Request &req, httplib::Response &res) {
		ApiRequest r;
		r.method = req.method;
		r.path = req.path;
		r.body = req.body;
		r.pin = req.get_header_value("X-HimitsuKao-Pin");
		r.remote_ip = req.remote_addr;
		const ApiResponse out = api->handle(r);
		res.status = out.status;
		res.set_content(out.body, out.content_type);
		if (out.no_store)
			res.set_header("Cache-Control", "no-store");
		if (out.retry_after_s > 0)
			res.set_header("Retry-After", std::to_string(out.retry_after_s));
		res.set_header("X-Content-Type-Options", "nosniff");
	};
	server->Get(".*", handler);
	server->Post(".*", handler);
	server->Delete(".*", handler);
	server->Patch(".*", handler);
	server->set_exception_handler([](const httplib::Request &, httplib::Response &res, std::exception_ptr) {
		res.status = 500;
		res.set_content("{\"error\":\"internal error\"}", "application/json");
	});

	int bound_port = port;
	bool bound = true;
	if (port == 0) {
		bound_port = server->bind_to_any_port(host);
		bound = bound_port > 0;
	} else {
		bound = server->bind_to_port(host, port);
	}
	if (!bound) {
		if (error)
			*error = "cannot bind " + host + ":" + std::to_string(port) + " (port busy?)";
		return false;
	}
	port_ = bound_port;
	server_ = std::move(server);
	running_ = true;
	httplib::Server *raw = server_.get();
	thread_ = std::thread([raw] { raw->listen_after_bind(); });
	for (int i = 0; i < 200 && !server_->is_running(); ++i) // bounded wait for the listen loop
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	return true;
}

void HttpServer::stop()
{
	if (!server_)
		return;
	server_->stop();
	if (thread_.joinable())
		thread_.join();
	server_.reset();
	running_ = false;
}

} // namespace himitsukao

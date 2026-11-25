#pragma once

#include <memory>
#include <string>
#include <thread>

#include "api.h"

namespace httplib {
class Server;
}

namespace himitsukao {

// Thin cpp-httplib wrapper: every request is translated to an ApiRequest and answered by the ApiHandler.
class HttpServer {
public:
	explicit HttpServer(ApiHandler &api);
	~HttpServer();
	HttpServer(const HttpServer &) = delete;
	HttpServer &operator=(const HttpServer &) = delete;

	// Binds synchronously (so a busy port is reported right away) then serves on a background thread.
	// port 0 = any free port. Returns false and fills `error` if binding fails.
	bool start(const std::string &host, int port, std::string *error = nullptr);
	// Stops accepting and joins the thread. Safe to call twice.
	void stop();

	bool running() const { return running_; }
	int port() const { return port_; }

private:
	ApiHandler &api_;
	std::unique_ptr<httplib::Server> server_;
	std::thread thread_;
	bool running_ = false;
	int port_ = 0;
};

} // namespace himitsukao

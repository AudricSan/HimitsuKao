// Development tool: serves the phone page (src/web/index.html, read from disk on each start) with two FAKE faces,
// so the page and the API can be tried from a phone without OBS.
//
//   himitsukao_devserver [port] [path/to/index.html] [--lan]      access code: devcode234
//
// By default it listens on 127.0.0.1 only; --lan listens on every interface (the same IP allowlist as the plugin
// still applies: LAN, Tailscale and loopback clients only).
//
// Face "A" and face "B" are synthetic: enrolling one of them un-masks it (its badge becomes a name), removing the
// person from the "Known" list masks it again. Nothing is written to disk.
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <opencv2/imgproc.hpp>

#include "api.h"
#include "http-server.h"
#include "image-utils.h"

using namespace himitsukao;

static Embedding fake_embedding(unsigned seed)
{
	std::mt19937 rng(seed);
	std::normal_distribution<float> nd(0.f, 1.f);
	Embedding e(128);
	for (auto &x : e)
		x = nd(rng);
	return l2_normalize(e);
}

int main(int argc, char **argv)
{
	bool lan = false;
	std::vector<std::string> args;
	for (int i = 1; i < argc; ++i) {
		if (std::string(argv[i]) == "--lan")
			lan = true;
		else
			args.push_back(argv[i]);
	}
	const int port = args.size() > 0 ? std::atoi(args[0].c_str()) : 8787;
	const std::string page_path = args.size() > 1 ? args[1] : "src/web/index.html";
	std::ifstream in(page_path, std::ios::binary);
	if (!in) {
		std::fprintf(stderr, "cannot read %s (pass the path to src/web/index.html as 2nd argument)\n",
			     page_path.c_str());
		return 1;
	}
	std::stringstream page;
	page << in.rdbuf();

	auto t0 = std::chrono::steady_clock::now();
	auto now_ms = [t0] {
		return static_cast<int64_t>(
			std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count());
	};

	HimitsukaoService service;
	IdentityManager manager(service.database(), service.board(), IdentityConfig{});
	service.register_instance("Fake camera", &manager);
	ApiHandler api(service, page.str(), "devcode234", now_ms);
	HttpServer server(api);
	std::string error;
	if (!server.start(lan ? "0.0.0.0" : "127.0.0.1", port, &error)) {
		std::fprintf(stderr, "%s\n", error.c_str());
		return 1;
	}
	std::printf("dev server on http://localhost:%d  PIN 123456  (Ctrl+C to stop)\n", server.port());

	FaceTracker tracker;
	cv::Mat frame(480, 640, CV_8UC3, cv::Scalar(40, 40, 40));
	cv::circle(frame, {160, 240}, 70, cv::Scalar(60, 120, 230), -1); // face A
	cv::circle(frame, {480, 240}, 70, cv::Scalar(90, 200, 90), -1);  // face B
	cv::putText(frame, "A", {135, 265}, cv::FONT_HERSHEY_SIMPLEX, 2.0, cv::Scalar(255, 255, 255), 4);
	cv::putText(frame, "B", {455, 265}, cv::FONT_HERSHEY_SIMPLEX, 2.0, cv::Scalar(255, 255, 255), 4);
	const std::vector<Rect> faces = {Rect{90.f / 640, 170.f / 480, 140.f / 640, 140.f / 480},
					 Rect{410.f / 640, 170.f / 480, 140.f / 640, 140.f / 480}};
	for (;;) {
		const int64_t t = now_ms();
		auto ids = tracker.update(faces, t);
		manager.process(
			tracker, ids, t, [](size_t i) { return fake_embedding(100 + static_cast<unsigned>(i)); },
			[&](size_t i) { return face_thumbnail_b64(frame, faces[i]); });
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
	}
}

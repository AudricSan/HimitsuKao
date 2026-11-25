// Plain-assert tests (no framework). Built when HIMITSUKAO_BUILD_TESTS=ON, run with ctest.
#include <cmath>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <map>
#include <random>
#include <sstream>
#include <thread>
#include <chrono>
#include <functional>
#include <string>
#include <vector>

#include <opencv2/core.hpp>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "api.h"
#include "auth.h"
#include "face-database.h"
#include "face-detector.h"
#include "face-recognizer.h"
#include "himitsukao-service.h"
#include "identity-manager.h"
#include "http-server.h"
#include "ip-filter.h"
#include "image-utils.h"
#include "runtime-state.h"
#include "server-binding.h"
#include "server-config.h"
#include "face-tracker.h"

using namespace himitsukao;

static int g_failures = 0;

#define CHECK(cond)                                                                            \
	do {                                                                                   \
		if (!(cond)) {                                                                 \
			std::fprintf(stderr, "  CHECK FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond); \
			++g_failures;                                                          \
		}                                                                              \
	} while (0)

static bool approx(float a, float b, float eps = 1e-4f)
{
	return std::fabs(a - b) < eps;
}

static void test_iou()
{
	Rect a{0.1f, 0.1f, 0.2f, 0.2f};
	CHECK(approx(rect_iou(a, a), 1.f));
	CHECK(approx(rect_iou(a, Rect{0.5f, 0.5f, 0.1f, 0.1f}), 0.f));
	// Half overlap: inter = 0.1*0.2, union = 2*0.04 - 0.02
	CHECK(approx(rect_iou(a, Rect{0.2f, 0.1f, 0.2f, 0.2f}), 0.02f / 0.06f));
}

static void test_new_track_and_id_stability()
{
	FaceTracker tr;
	auto ids = tr.update({Rect{0.2f, 0.2f, 0.1f, 0.1f}, Rect{0.6f, 0.5f, 0.1f, 0.1f}}, 0);
	CHECK(ids.size() == 2);
	CHECK(ids[0] != ids[1]);
	CHECK(tr.tracks().size() == 2);

	// Both faces move slightly each frame: ids must stay the same.
	for (int f = 1; f <= 10; ++f) {
		const float dx = 0.003f * static_cast<float>(f);
		auto next = tr.update({Rect{0.2f + dx, 0.2f, 0.1f, 0.1f}, Rect{0.6f - dx, 0.5f, 0.1f, 0.1f}}, f * 66);
		CHECK(next[0] == ids[0]);
		CHECK(next[1] == ids[1]);
	}
	CHECK(tr.tracks().size() == 2);
	CHECK(tr.tracks()[0].hits == 11);
}

static void test_id_stability_when_detection_order_changes()
{
	FaceTracker tr;
	auto ids = tr.update({Rect{0.1f, 0.1f, 0.1f, 0.1f}, Rect{0.7f, 0.7f, 0.1f, 0.1f}}, 0);
	auto swapped = tr.update({Rect{0.7f, 0.7f, 0.1f, 0.1f}, Rect{0.1f, 0.1f, 0.1f, 0.1f}}, 66);
	CHECK(swapped[0] == ids[1]);
	CHECK(swapped[1] == ids[0]);
}

static void test_new_face_gets_new_track()
{
	FaceTracker tr;
	auto first = tr.update({Rect{0.1f, 0.1f, 0.1f, 0.1f}}, 0);
	auto second = tr.update({Rect{0.1f, 0.1f, 0.1f, 0.1f}, Rect{0.6f, 0.6f, 0.1f, 0.1f}}, 66);
	CHECK(second[0] == first[0]);
	CHECK(second[1] != first[0]);
	CHECK(tr.tracks().size() == 2);
}

static void test_smoothing()
{
	TrackerConfig cfg;
	cfg.ema_alpha = 0.5f;
	FaceTracker tr(cfg);
	tr.update({Rect{0.2f, 0.2f, 0.2f, 0.2f}}, 0);
	tr.update({Rect{0.24f, 0.2f, 0.2f, 0.2f}}, 66);
	CHECK(approx(tr.tracks()[0].rect.x, 0.22f));
}

static void test_hold()
{
	FaceTracker tr;
	tr.update({Rect{0.4f, 0.4f, 0.1f, 0.1f}}, 1000);
	CHECK(tr.masks(1000, 300, 1.f).size() == 1);
	CHECK(tr.masks(1300, 300, 1.f).size() == 1); // exactly hold_ms: still masked
	CHECK(tr.masks(1301, 300, 1.f).empty());     // past hold_ms: not masked...
	CHECK(tr.tracks().size() == 1);              // ...but the track is still alive for re-matching
	CHECK(tr.masks(1301, 500, 1.f).size() == 1); // longer hold keeps it
	CHECK(tr.masks(1000, 0, 1.f).size() == 1);   // hold 0: only the current frame
	CHECK(tr.masks(1001, 0, 1.f).empty());
}

static void test_reappearing_face_keeps_id_within_max_age()
{
	FaceTracker tr;
	auto first = tr.update({Rect{0.4f, 0.4f, 0.1f, 0.1f}}, 0);
	tr.update({}, 500);
	auto back = tr.update({Rect{0.405f, 0.4f, 0.1f, 0.1f}}, 900);
	CHECK(back[0] == first[0]);
}

static void test_expiry()
{
	TrackerConfig cfg;
	cfg.max_age_ms = 1000;
	FaceTracker tr(cfg);
	auto first = tr.update({Rect{0.4f, 0.4f, 0.1f, 0.1f}}, 0);
	tr.update({}, 1000);
	CHECK(tr.tracks().size() == 1); // exactly max_age: kept
	tr.update({}, 1001);
	CHECK(tr.tracks().empty());
	auto again = tr.update({Rect{0.4f, 0.4f, 0.1f, 0.1f}}, 1100);
	CHECK(again[0] != first[0]); // new id after expiry
}

static void test_padding()
{
	Rect r = pad_rect(Rect{0.4f, 0.4f, 0.2f, 0.2f}, 1.5f);
	CHECK(approx(r.x, 0.35f) && approx(r.y, 0.35f) && approx(r.w, 0.3f) && approx(r.h, 0.3f));

	// Clamped to the image bounds.
	Rect c = pad_rect(Rect{0.0f, 0.9f, 0.2f, 0.1f}, 2.0f);
	CHECK(c.x >= 0.f && c.y >= 0.f);
	CHECK(c.x + c.w <= 1.f + 1e-6f && c.y + c.h <= 1.f + 1e-6f);
	CHECK(approx(c.x, 0.f));
	CHECK(approx(c.y + c.h, 1.f));
	CHECK(approx(c.w, 0.3f)); // from -0.1 .. 0.3 clamped to 0 .. 0.3

	FaceTracker tr;
	tr.update({Rect{0.9f, 0.9f, 0.1f, 0.1f}}, 0);
	auto m = tr.masks(0, 300, 1.3f);
	CHECK(m.size() == 1);
	CHECK(m[0].x + m[0].w <= 1.f + 1e-6f);
	CHECK(m[0].y + m[0].h <= 1.f + 1e-6f);
}

static void test_detector_smoke()
{
#ifdef HIMITSUKAO_TEST_MODEL
	FaceDetector det(HIMITSUKAO_TEST_MODEL);
	cv::Mat blank(480, 640, CV_8UC4, cv::Scalar(128, 128, 128, 255));
	CHECK(det.detect(blank).empty());
	cv::Mat noise_bgr(360, 640, CV_8UC3);
	cv::randu(noise_bgr, cv::Scalar::all(0), cv::Scalar::all(255));
	det.detect(noise_bgr); // different size, must not throw
	CHECK(det.detect(cv::Mat()).empty());
	bool threw = false;
	try {
		FaceDetector bad("this/file/does/not/exist.onnx");
	} catch (const std::exception &) {
		threw = true;
	}
	CHECK(threw);
#endif
}

#include "test-identity.inc"
#include "test-api.inc"
#include "test-security.inc"
#include "test-ui.inc"

int main()
{
	struct T {
		const char *name;
		std::function<void()> fn;
	};
	const std::vector<T> tests = {
		{"iou", test_iou},
		{"new_track_and_id_stability", test_new_track_and_id_stability},
		{"id_stability_when_order_changes", test_id_stability_when_detection_order_changes},
		{"new_face_gets_new_track", test_new_face_gets_new_track},
		{"smoothing", test_smoothing},
		{"hold", test_hold},
		{"reappearing_face", test_reappearing_face_keeps_id_within_max_age},
		{"expiry", test_expiry},
		{"padding", test_padding},
		{"detector_smoke", test_detector_smoke},
		{"cosine", test_cosine},
		{"db_add_match_remove", test_db_add_match_remove},
		{"db_save_load_session", test_db_save_load_session},
		{"db_corrupt_file", test_db_corrupt_file},
		{"recognition_sticks_and_unmasks", test_recognition_sticks_and_unmasks},
		{"unknown_recheck_interval", test_unknown_recheck_interval},
		{"enrollment_flow", test_enrollment_flow},
		{"enrollment_saves_persistent_only", test_enrollment_saves_persistent_only},
		{"enrollment_expiry", test_enrollment_expiry},
		{"masks_skip_recognised_tracks", test_masks_skip_recognised_tracks},
		{"service_remove_remasks_and_enroll", test_service_remove_remasks_and_enroll},
		{"thumbnail", test_thumbnail},
		{"recognizer_smoke", test_recognizer_smoke},
		{"constant_time_and_access_code", test_constant_time_and_access_code},
		{"code_lockout", test_code_lockout},
		{"runtime_state", test_runtime_state},
		{"masks_by_mode_helpers", test_masks_by_mode_helpers},
		{"person_name_validation", test_person_name_validation},
		{"api_auth_and_static", test_api_auth_and_static},
		{"api_state_mode_style", test_api_state_mode_style},
		{"api_body_limit", test_api_body_limit},
		{"api_live_known_enroll_remove", test_api_live_known_enroll_remove},
		{"server_config", test_server_config},
		{"http_server_smoke", test_http_server_smoke},
		{"ip_allowlist", test_ip_allowlist},
		{"host_header", test_host_header},
		{"api_ip_and_host_filtering", test_api_ip_and_host_filtering},
		{"global_failure_budget", test_global_failure_budget},
		{"auth_table_cap_keeps_partial_counts", test_auth_table_cap_keeps_partial_counts},
		{"api_global_lock_logs_once", test_api_global_lock_logs_once},
		{"enroll_queue_cap_and_board_prune", test_enroll_queue_cap_and_board_prune},
		{"config_enabled_and_migration", test_config_enabled_and_migration},
		{"http_host_and_limits", test_http_host_and_limits},
		{"server_binding_decision", test_server_binding_decision},
		{"box_colors", test_box_colors},
		{"debug_boxes", test_debug_boxes},
	};
	for (const auto &t : tests) {
		const int before = g_failures;
		try {
			t.fn();
		} catch (const std::exception &e) {
			std::fprintf(stderr, "  EXCEPTION in %s: %s\n", t.name, e.what());
			++g_failures;
		}
		std::printf("[%s] %s\n", g_failures == before ? " OK " : "FAIL", t.name);
	}
	std::printf("%d failure(s)\n", g_failures);
	return g_failures == 0 ? 0 : 1;
}

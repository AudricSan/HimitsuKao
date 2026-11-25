// Security hardening tests (IP allowlist, Host check, failure budgets, caps, config migration). Included by
// test-main.cpp after test-api.inc (uses ApiFixture and the identity test helpers).

static void test_ip_allowlist()
{
	for (const char *ip : {"127.0.0.1", "127.255.255.254", "10.0.0.1", "10.255.1.2", "172.16.0.1", "172.31.255.255",
			       "192.168.0.1", "192.168.1.77", "100.64.0.1", "100.100.100.100", "100.127.255.255", "::1",
			       "fc00::1", "fd7a:115c:a1e0::1", "fdff:ffff::5", "::ffff:192.168.1.5", "::ffff:10.1.2.3",
			       "::ffff:127.0.0.1", "::ffff:100.64.0.9"})
		CHECK(is_allowed_client_ip(ip));
	for (const char *ip : {"8.8.8.8", "1.1.1.1", "172.15.255.255", "172.32.0.1", "100.63.255.255", "100.128.0.1",
			       "192.167.1.1", "192.169.1.1", "11.0.0.1", "169.254.1.1", "0.0.0.0", "255.255.255.255",
			       "::", "fe80::1", "fe00::1", "2001:db8::1", "2606:4700::1111", "::ffff:8.8.8.8",
			       "::ffff:172.32.0.1", "", "garbage", "1.2.3", "1.2.3.4.5", "256.1.1.1", "10.0.0.1 ",
			       " 10.0.0.1", "10.0.0.1/8", "localhost", "192.168.1"})
		CHECK(!is_allowed_client_ip(ip));
	CHECK(is_loopback_ip("127.0.0.1") && is_loopback_ip("::1") && is_loopback_ip("::ffff:127.0.0.1"));
	CHECK(!is_loopback_ip("192.168.1.2") && !is_loopback_ip("8.8.8.8") && !is_loopback_ip("fd00::1") &&
	      !is_loopback_ip(""));
}

static void test_host_header()
{
	for (const char *h : {"localhost", "LocalHost:8787", "127.0.0.1:8787", "192.168.1.5:8787", "10.0.0.1",
			      "[::1]:8787", "[fd7a:115c:a1e0::1]", "100.64.0.1:80"})
		CHECK(is_acceptable_host_header(h));
	for (const char *h : {"", "evil.com", "evil.com:8787", "192.168.1.5.evil.com", "localhost.evil.com", "1.2.3",
			      "[::1", "::1", "10.0.0.1:abc", "10.0.0.1:", "example.org:80", "[evil]:80", "a b",
			      "10.0.0.1:123456"})
		CHECK(!is_acceptable_host_header(h));
}

static void test_api_ip_and_host_filtering()
{
	ApiFixture fx;
	auto from = [&](const std::string &ip, const std::string &path, const std::string &host = "10.0.0.2:8787") {
		ApiRequest r;
		r.method = "GET";
		r.path = path;
		r.code = "k7m2q9x4cd";
		r.remote_ip = ip;
		r.host = host;
		return fx.api.handle(r);
	};
	CHECK(from("8.8.8.8", "/").status == 403); // the page itself is protected too
	CHECK(from("8.8.8.8", "/api/state").status == 403);
	CHECK(from("2001:db8::1", "/api/state").status == 403);
	CHECK(from("garbage", "/").status == 403);
	CHECK(from("192.168.1.9", "/").status == 200);
	CHECK(from("100.64.1.1", "/api/state").status == 200); // Tailscale / CGNAT
	CHECK(from("fd7a:115c:a1e0::1", "/api/state", "[fd7a:115c:a1e0::1]:8787").status == 200);
	CHECK(from("127.0.0.1", "/api/state", "localhost:8787").status == 200);

	// DNS rebinding: a name that resolves to a LAN address is not an IP literal.
	CHECK(from("192.168.1.9", "/", "attacker.example:8787").status == 421);
	CHECK(from("192.168.1.9", "/api/state", "attacker.example").status == 421);
	CHECK(from("192.168.1.9", "/", "").status == 421);
	CHECK(from("8.8.8.8", "/", "attacker.example").status == 403); // address first
	// A refused request never reaches the code check: it records no failure.
	CHECK(fx.api.auth().tracked_ips() == 0);
}

static void test_global_failure_budget()
{
	CodeAuthenticator auth("k7m2q9x4cd");
	int64_t now = 0;
	// 30 wrong codes from 30 different addresses: still denied individually.
	for (int i = 0; i < 30; ++i)
		CHECK(auth.check("10.0.1." + std::to_string(i), "wrong", now += 1000) == AuthResult::Denied);
	CHECK(!auth.take_global_lock_event());
	CHECK(auth.check("10.0.2.1", "k7m2q9x4cd", now) == AuthResult::Ok);
	// The 31st failure inside 10 minutes locks every remote client.
	CHECK(auth.check("10.0.1.99", "wrong", now) == AuthResult::Denied);
	CHECK(auth.take_global_lock_event());
	CHECK(!auth.take_global_lock_event()); // reported once
	CHECK(auth.check("10.0.2.1", "k7m2q9x4cd", now + 1000) == AuthResult::Locked); // even the right code
	CHECK(auth.check("192.168.5.5", "k7m2q9x4cd", now + 1000) == AuthResult::Locked);
	CHECK(auth.retry_after_ms("192.168.5.5", now + 1000) > 500000);
	// Loopback is exempt (the streamer at the PC cannot be locked out).
	CHECK(auth.check("127.0.0.1", "k7m2q9x4cd", now + 1000, true) == AuthResult::Ok);
	// After 10 minutes everything works again.
	CHECK(auth.check("10.0.2.1", "k7m2q9x4cd", now + 600001) == AuthResult::Ok);

	// Failures older than the 10 minute window do not add up.
	CodeAuthenticator slow("k7m2q9x4cd");
	for (int i = 0; i < 100; ++i)
		CHECK(slow.check("10.0.3." + std::to_string(i % 200), "wrong", int64_t(i) * 30000) == AuthResult::Denied);
	CHECK(!slow.take_global_lock_event());
}

static void test_auth_table_cap_keeps_partial_counts()
{
	CodeAuthenticator::Limits limits;
	limits.max_entries = 8;
	limits.global_max_failures = 100000; // not under test here
	CodeAuthenticator auth("k7m2q9x4cd", limits);
	for (int i = 0; i < 3; ++i)
		CHECK(auth.check("10.9.9.9", "wrong", 100 + i) == AuthResult::Denied);
	// A flood of other addresses fills (and overflows) the table...
	for (int i = 0; i < 50; ++i)
		auth.check("10.8.0." + std::to_string(i), "wrong", 1000 + i);
	CHECK(auth.tracked_ips() <= 8);
	// ...and eviction goes by last activity (oldest first), never all at once.
	CHECK(auth.tracked_ips() >= 4);

	// While the table has room, partial counts accumulate across other traffic and are never wiped.
	CodeAuthenticator keep("k7m2q9x4cd", limits);
	for (int i = 0; i < 4; ++i)
		keep.check("10.9.9.9", "wrong", 5000 + i);
	for (int i = 0; i < 5; ++i)
		keep.check("10.6.0." + std::to_string(i), "wrong", 6000 + i);
	keep.check("10.9.9.9", "wrong", 6100); // 5th failure
	CHECK(keep.check("10.9.9.9", "k7m2q9x4cd", 6101) == AuthResult::Locked);

	// Expired entries are pruned when the table is full.
	CodeAuthenticator tiny("k7m2q9x4cd", limits);
	for (int i = 0; i < 8; ++i)
		tiny.check("10.7.0." + std::to_string(i), "wrong", 10);
	CHECK(tiny.tracked_ips() == 8);
	tiny.check("10.7.1.1", "wrong", 10 + 600001);
	CHECK(tiny.tracked_ips() == 1);
}

static void test_api_global_lock_logs_once()
{
	ApiFixture fx;
	for (int i = 0; i < 40; ++i) {
		ApiRequest r;
		r.method = "GET";
		r.path = "/api/state";
		r.code = "wrong";
		r.remote_ip = "10.1.0." + std::to_string(i + 1);
		r.host = "10.0.0.2:8787";
		fx.api.handle(r);
	}
	CHECK(fx.logs.size() == 1);
	CHECK(fx.logs[0].find("k7m2q9x4cd") == std::string::npos && fx.logs[0].find("wrong") == std::string::npos);
	CHECK(fx.call("GET", "/api/state", {}, "k7m2q9x4cd", "10.77.0.1").status == 429);
	CHECK(fx.call("GET", "/api/state", {}, "k7m2q9x4cd", "127.0.0.1").status == 200);
}

static void test_enroll_queue_cap_and_board_prune()
{
	ApiFixture fx;
	const std::string body = R"({"instance":")" + fx.key + R"(","track_id":1,"name":"Pat"})";
	for (size_t i = 0; i < kMaxPendingEnrollments; ++i)
		CHECK(fx.call("POST", "/api/known", body).status == 202);
	auto busy = fx.call("POST", "/api/known", body);
	CHECK(busy.status == 429 && busy.retry_after_s > 0);
	CHECK(fx.mgr.pending_requests() == kMaxPendingEnrollments);
	// The worker finishes them (the track does not exist: they fail), the queue is free again.
	FaceTracker tracker;
	fx.mgr.process(tracker, {}, 1000, nullptr, nullptr);
	CHECK(fx.mgr.pending_requests() == 0);
	CHECK(fx.call("POST", "/api/known", body).status == 202);

	EnrollmentBoard board;
	const std::string done = board.new_request();
	const std::string pending = board.new_request();
	board.set(done, EnrollState::Done, "p1");
	std::this_thread::sleep_for(std::chrono::milliseconds(5));
	board.prune(std::chrono::milliseconds(0));
	CHECK(board.get(done).state == EnrollState::Unknown);    // finished and old: forgotten
	CHECK(board.get(pending).state == EnrollState::Pending); // still in flight: kept
	CHECK(board.size() == 1);
	board.set(pending, EnrollState::Failed, {}, "x");
	board.prune(std::chrono::minutes(5));
	CHECK(board.get(pending).state == EnrollState::Failed); // recent: kept
}

static void test_config_enabled_and_migration()
{
	const std::string dir = temp_dir("server_cfg2");
	const std::string path = dir + "/server.json";
	{
		std::ofstream out(path);
		out << R"({"port": 8800, "pin": "123456", "style": "solid", "strength": 20})";
	}
	ServerConfig c;
	auto r = c.load(path);
	CHECK(r.ok && r.existed && r.needs_save);
	CHECK(c.code.size() == 10 && c.code != "123456"); // the 6-digit PIN is replaced
	CHECK(!c.enabled);                                // opt-in: off unless the file says otherwise
	CHECK(c.port == 8800 && c.style == Style::Solid && c.strength == 20);
	CHECK(c.save(path));
	CHECK(read_text(path).find("\"pin\"") == std::string::npos);
	CHECK(read_text(path).find(c.code) != std::string::npos);

	ServerConfig again;
	r = again.load(path);
	CHECK(r.ok && !r.needs_save && again.code == c.code); // a valid code is kept

	c.enabled = true;
	CHECK(c.save(path));
	ServerConfig on;
	on.load(path);
	CHECK(on.enabled);

#ifndef _WIN32
	namespace fs = std::filesystem;
	CHECK((fs::status(path).permissions() & (fs::perms::group_all | fs::perms::others_all)) == fs::perms::none);
	FaceDatabase db;
	db.load(dir + "/known.json");
	db.add_person("A", false, synth_embedding(1), "");
	CHECK(db.save());
	CHECK((fs::status(dir + "/known.json").permissions() & (fs::perms::group_all | fs::perms::others_all)) ==
	      fs::perms::none);
#endif
}

static void test_http_host_and_limits()
{
	HimitsukaoService svc;
	ApiHandler api(svc, "<html>hi</html>", "p4r7s2u9ye", [] { return int64_t(0); });
	HttpServer server(api);
	CHECK(server.start("127.0.0.1", 0));
	httplib::Client cli("127.0.0.1", server.port());
	cli.set_connection_timeout(3);
	cli.set_read_timeout(3);
	// A forged Host header (DNS rebinding) is refused with 421.
	httplib::Headers evil{{"Host", "attacker.example"}};
	auto r = cli.Get("/", evil);
	CHECK(r && r->status == 421);
	auto ok = cli.Get("/");
	CHECK(ok && ok->status == 200);
	server.stop();
}

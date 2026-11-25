// Step 5-7 tests (auth, API, runtime state, server config, HTTP smoke). Included by test-main.cpp after
// test-identity.inc (uses its helpers: synth_embedding, temp_dir, read_text, kFace).

static void test_constant_time_and_access_code()
{
	CHECK(constant_time_equals("123456", "123456"));
	CHECK(!constant_time_equals("123456", "123457"));
	CHECK(!constant_time_equals("123456", "12345"));
	CHECK(!constant_time_equals("", "1"));
	CHECK(constant_time_equals("", ""));
	for (int i = 0; i < 200; ++i) {
		const std::string code = generate_access_code();
		CHECK(code.size() == 10);
		CHECK(code.find_first_not_of("abcdefghjkmnpqrstuvwxyz23456789") == std::string::npos); // no 0 o 1 l i
		CHECK(is_valid_access_code(code));
	}
	CHECK(generate_access_code() != generate_access_code());
	CHECK(!is_valid_access_code("123456")); // an old 6-digit PIN
}

static void test_code_lockout()
{
	CodeAuthenticator auth("424242");
	CHECK(auth.check("1.1.1.1", "424242", 0) == AuthResult::Ok);
	CHECK(auth.check("1.1.1.1", "", 0) == AuthResult::Denied); // missing PIN: denied, not counted
	for (int i = 0; i < 20; ++i)
		CHECK(auth.check("2.2.2.2", "", 0) == AuthResult::Denied);
	CHECK(auth.check("2.2.2.2", "424242", 0) == AuthResult::Ok);

	// 4 wrong PINs, then a good one: the counter resets.
	for (int i = 0; i < 4; ++i)
		CHECK(auth.check("3.3.3.3", "000000", 10) == AuthResult::Denied);
	CHECK(auth.check("3.3.3.3", "424242", 10) == AuthResult::Ok);
	for (int i = 0; i < 4; ++i)
		CHECK(auth.check("3.3.3.3", "000000", 20) == AuthResult::Denied);

	// 5 wrong PINs in a row: the 5th is still "denied", then the IP is locked for 60 s (even with the right PIN).
	for (int i = 0; i < 5; ++i)
		CHECK(auth.check("4.4.4.4", "000000", 100) == AuthResult::Denied);
	CHECK(auth.check("4.4.4.4", "424242", 101) == AuthResult::Locked);
	CHECK(auth.check("4.4.4.4", "000000", 30000) == AuthResult::Locked);
	CHECK(auth.retry_after_ms("4.4.4.4", 30100) == 30000);
	CHECK(auth.check("5.5.5.5", "424242", 101) == AuthResult::Ok); // other addresses are not affected
	CHECK(auth.check("4.4.4.4", "424242", 60101) == AuthResult::Ok); // lock expired
	CHECK(auth.retry_after_ms("4.4.4.4", 60101) == 0);

	auth.set_code("111111");
	CHECK(auth.check("6.6.6.6", "424242", 0) == AuthResult::Denied);
	CHECK(auth.check("6.6.6.6", "111111", 0) == AuthResult::Ok);
}

static void test_runtime_state()
{
	RuntimeState rt;
	CHECK(rt.mode() == Mode::Normal && rt.style() == Style::Pixel && rt.strength() == 50);
	const uint32_t v = rt.version();
	rt.set_mode(Mode::FrameAll);
	CHECK(rt.mode() == Mode::FrameAll && rt.version() != v);
	rt.set_style(Style::Gaussian, 500);
	CHECK(rt.style() == Style::Gaussian && rt.strength() == 100);
	rt.set_style(Style::Solid, -3);
	CHECK(rt.strength() == 1);

	Mode m;
	CHECK(parse_mode("faces_all", m) && m == Mode::FacesAll);
	CHECK(parse_mode("off", m) && m == Mode::Off);
	CHECK(!parse_mode("OFF", m) && !parse_mode("", m));
	Style s;
	CHECK(parse_style("gaussian", s) && s == Style::Gaussian);
	CHECK(!parse_style("blur", s));
	for (Mode x : {Mode::Normal, Mode::FacesAll, Mode::FrameAll, Mode::Off}) {
		CHECK(parse_mode(mode_name(x), m) && m == x);
	}
}

static void test_masks_by_mode_helpers()
{
	// Known people are masked again with include_known (the "faces_all" emergency mode).
	FaceTracker tr;
	tr.update({kFace, kFace2}, 0);
	tr.tracks()[0].person_id = "abc";
	CHECK(masks_from_tracks(tr.tracks(), 0, 300, 1.f).size() == 1);
	CHECK(masks_from_tracks(tr.tracks(), 0, 300, 1.f, true).size() == 2);

	FaceDatabase db;
	EnrollmentBoard board;
	IdentityManager mgr(db, board);
	auto ids = tr.update({kFace, kFace2}, 10);
	mgr.process(tr, ids, 10, nullptr, nullptr);
	CHECK(mgr.masks(10, 300, 1.f).size() == 1);
	CHECK(mgr.masks(10, 300, 1.f, true).size() == 2);
}

static void test_person_name_validation()
{
	std::string out;
	CHECK(normalize_person_name("  Bob \n", out) && out == "Bob");
	CHECK(!normalize_person_name("", out));
	CHECK(!normalize_person_name("   ", out));
	CHECK(normalize_person_name(std::string(40, 'a'), out));
	CHECK(!normalize_person_name(std::string(41, 'a'), out));
	CHECK(normalize_person_name("Ren\xC3\xA9""e", out) && out == "Ren\xC3\xA9""e"); // accents count as 1 character
	std::string accents;
	for (int i = 0; i < 40; ++i)
		accents += "\xC3\xA9"; // 40 x e-acute = 80 bytes, 40 characters
	CHECK(normalize_person_name(accents, out));
	CHECK(!normalize_person_name("a\x01" "b", out));
}

struct ApiFixture {
	HimitsukaoService svc;
	IdentityManager mgr;
	int64_t now = 100000;
	std::string host = "10.0.0.2:8787";
	std::vector<std::string> logs;
	ApiHandler api;
	std::string key;
	int style_saves = 0;

	ApiFixture()
		: mgr(svc.database(), svc.board(), IdentityConfig{}),
		  api(svc, "<html>phone</html>", "k7m2q9x4cd", [this] { return now; },
		      [this](const std::string &m) { logs.push_back(m); })
	{
		key = svc.register_instance("Cam 1", &mgr);
		svc.set_style_listener([this] { ++style_saves; });
	}
	~ApiFixture() { svc.unregister_instance(key); }

	ApiResponse call(const std::string &method, const std::string &path, const std::string &body = {},
			 const std::string &code = "k7m2q9x4cd", const std::string &ip = "10.0.0.2")
	{
		ApiRequest r;
		r.method = method;
		r.path = path;
		r.body = body;
		r.code = code;
		r.host = host;
		r.remote_ip = ip;
		return api.handle(r);
	}
	nlohmann::json json_of(const ApiResponse &r) { return nlohmann::json::parse(r.body); }
};

static void test_api_auth_and_static()
{
	ApiFixture fx;
	auto page = fx.call("GET", "/", {}, "");
	CHECK(page.status == 200 && page.body == "<html>phone</html>" && page.content_type.find("text/html") == 0);
	CHECK(fx.call("POST", "/", {}, "").status == 405);
	CHECK(fx.call("GET", "/nothing", {}, "").status == 404);

	CHECK(fx.call("GET", "/api/state", {}, "").status == 401);
	CHECK(fx.call("GET", "/api/state", {}, "000000").status == 401);
	auto ok = fx.call("GET", "/api/state");
	CHECK(ok.status == 200 && ok.no_store);

	// 5 wrong PINs then 429 with Retry-After, even for the right PIN; other IPs still work.
	for (int i = 0; i < 5; ++i)
		CHECK(fx.call("GET", "/api/state", {}, "999999", "10.9.9.9").status == 401);
	auto locked = fx.call("GET", "/api/state", {}, "k7m2q9x4cd", "10.9.9.9");
	CHECK(locked.status == 429 && locked.retry_after_s > 0 && locked.retry_after_s <= 60);
	CHECK(fx.call("GET", "/api/state").status == 200);
	fx.now += 61000;
	CHECK(fx.call("GET", "/api/state", {}, "k7m2q9x4cd", "10.9.9.9").status == 200);

	fx.api.set_code("h5n8v3w6tb");
	CHECK(fx.call("GET", "/api/state").status == 401);
	CHECK(fx.call("GET", "/api/state", {}, "h5n8v3w6tb").status == 200);
}

static void test_api_state_mode_style()
{
	ApiFixture fx;
	auto st = fx.json_of(fx.call("GET", "/api/state"));
	CHECK(st["mode"] == "normal" && st["style"] == "pixel" && st["strength"] == 50);

	CHECK(fx.call("POST", "/api/mode", R"({"mode":"frame_all"})").status == 200);
	CHECK(fx.svc.runtime().mode() == Mode::FrameAll);
	CHECK(fx.json_of(fx.call("GET", "/api/state"))["mode"] == "frame_all");
	CHECK(fx.call("POST", "/api/mode", R"({"mode":"faces_all"})").status == 200);
	CHECK(fx.call("POST", "/api/mode", R"({"mode":"off"})").status == 200);
	CHECK(fx.svc.runtime().mode() == Mode::Off);
	CHECK(fx.call("POST", "/api/mode", R"({"mode":"bogus"})").status == 400);
	CHECK(fx.call("POST", "/api/mode", "not json").status == 400);
	CHECK(fx.call("POST", "/api/mode", "{}").status == 400);
	CHECK(fx.svc.runtime().mode() == Mode::Off); // unchanged by the bad calls
	CHECK(fx.call("GET", "/api/mode").status == 405);
	CHECK(fx.call("POST", "/api/mode", R"({"mode":"normal"})").status == 200);

	CHECK(fx.style_saves == 0);
	CHECK(fx.call("POST", "/api/style", R"({"style":"gaussian","strength":80})").status == 200);
	CHECK(fx.svc.runtime().style() == Style::Gaussian && fx.svc.runtime().strength() == 80);
	CHECK(fx.style_saves == 1); // the style change is persisted through the listener
	CHECK(fx.call("POST", "/api/style", R"({"style":"solid"})").status == 200); // strength optional
	CHECK(fx.svc.runtime().strength() == 80);
	CHECK(fx.call("POST", "/api/style", R"({"style":"pixel","strength":0})").status == 400);
	CHECK(fx.call("POST", "/api/style", R"({"style":"pixel","strength":101})").status == 400);
	CHECK(fx.call("POST", "/api/style", R"({"style":"pixel","strength":"x"})").status == 400);
	CHECK(fx.call("POST", "/api/style", R"({"style":"blur"})").status == 400);
	CHECK(fx.style_saves == 2);
	CHECK(fx.call("POST", "/api/mode", R"({"mode":"normal"})").status == 200);
	CHECK(fx.style_saves == 2); // mode changes are not persisted
}

static void test_api_body_limit()
{
	ApiFixture fx;
	const std::string big(kMaxBodyBytes + 1, 'x');
	CHECK(fx.call("POST", "/api/mode", big).status == 413);
	CHECK(fx.call("POST", "/api/mode", std::string(kMaxBodyBytes, ' ')).status == 400); // at the limit: parsed (and invalid)
	// Unauthenticated oversized bodies are still rejected as unauthorised first.
	CHECK(fx.call("POST", "/api/mode", big, "").status == 401);
}

static void test_api_live_known_enroll_remove()
{
	ApiFixture fx;
	FaceTracker tracker;
	std::map<size_t, Embedding> embs;
	embs[0] = synth_embedding(1);
	embs[1] = synth_embedding(2);
	auto step = [&](int64_t t, const std::vector<Rect> &rects) {
		auto ids = tracker.update(rects, t);
		fx.mgr.process(
			tracker, ids, t, [&](size_t i) { return embs[i]; }, [](size_t) { return std::string("QUJD"); });
		return ids;
	};
	// A known person first (so detection 0 is recognised), plus an unknown face.
	const std::string alice = fx.svc.database().add_person("Alice", true, synth_embedding(1), "");
	fx.now = 1000;
	auto ids = step(1000, {kFace, kFace2});

	auto live = fx.json_of(fx.call("GET", "/api/live"));
	CHECK(live.size() == 2);
	CHECK(live[0]["person_id"] == "" && live[0]["track_id"] == ids[1]); // unknown first
	CHECK(live[1]["person_id"] == alice && live[1]["name"] == "Alice");
	CHECK(live[0]["thumbnail"] == "data:image/jpeg;base64,QUJD");
	CHECK(live[0]["instance"] == fx.key && live[0]["label"] == "Cam 1");
	CHECK(live[0]["last_seen_ms_ago"] == 0);
	fx.now = 1400;
	CHECK(fx.json_of(fx.call("GET", "/api/live"))[0]["last_seen_ms_ago"] == 400);
	fx.now = 3000; // older than the 1.5 s window
	CHECK(fx.json_of(fx.call("GET", "/api/live")).empty());
	fx.now = 1400;

	// Enrollment validation
	const std::string track = std::to_string(ids[1]);
	auto post = [&](const std::string &body) { return fx.call("POST", "/api/known", body); };
	CHECK(post(R"({"instance":")" + fx.key + R"(","track_id":)" + track + R"(,"name":"   "})").status == 400);
	CHECK(post(R"({"instance":")" + fx.key + R"(","track_id":)" + track + R"(,"name":")" + std::string(41, 'z') + R"("})").status == 400);
	CHECK(post(R"({"instance":")" + fx.key + R"(","name":"Bob"})").status == 400);
	CHECK(post(R"({"track_id":1,"name":"Bob"})").status == 400);
	CHECK(post("[]").status == 400);
	CHECK(post(R"({"instance":"nope","track_id":1,"name":"Bob"})").status == 404);

	// Enrollment: the name is trimmed, the request goes pending, then done after the worker ran.
	auto accepted = fx.call("POST", "/api/known",
				R"({"instance":")" + fx.key + R"(","track_id":)" + track + R"(,"name":"  Bob ","session":true})");
	CHECK(accepted.status == 202);
	const std::string req = fx.json_of(accepted)["request_id"];
	CHECK(fx.json_of(fx.call("GET", "/api/enroll/" + req))["state"] == "pending");
	step(1500, {kFace, kFace2});
	auto status = fx.json_of(fx.call("GET", "/api/enroll/" + req));
	CHECK(status["state"] == "done" && !status["person_id"].get<std::string>().empty());
	const std::string bob = status["person_id"];
	CHECK(fx.call("GET", "/api/enroll/unknown").status == 404);

	auto known = fx.json_of(fx.call("GET", "/api/known"));
	CHECK(known.size() == 2);
	bool saw_bob = false;
	for (const auto &p : known)
		if (p["id"] == bob) {
			saw_bob = true;
			CHECK(p["name"] == "Bob" && p["session"] == true && p["created"].get<int64_t>() > 0);
		}
	CHECK(saw_bob);

	// Rename
	CHECK(fx.call("PATCH", "/api/known/" + bob, R"({"name":"Robert"})").status == 200);
	CHECK(fx.call("PATCH", "/api/known/" + bob, R"({"name":""})").status == 400);
	CHECK(fx.call("PATCH", "/api/known/nope", R"({"name":"X"})").status == 404);
	bool renamed = false;
	for (const auto &p : fx.json_of(fx.call("GET", "/api/known")))
		if (p["id"] == bob)
			renamed = p["name"] == "Robert";
	CHECK(renamed);

	// Removal re-masks immediately.
	CHECK(fx.mgr.masks(1500, 300, 1.f).empty());
	CHECK(fx.call("DELETE", "/api/known/" + bob).status == 200);
	CHECK(fx.call("DELETE", "/api/known/" + bob).status == 404);
	CHECK(fx.mgr.masks(1500, 300, 1.f).size() == 1);
	CHECK(fx.json_of(fx.call("GET", "/api/known")).size() == 1);
	CHECK(fx.call("PUT", "/api/known").status == 405);
	CHECK(fx.call("GET", "/api/unknown").status == 404);
}

static void test_server_config()
{
	const std::string dir = temp_dir("server_cfg");
	const std::string path = dir + "/server.json";
	ServerConfig c;
	auto r = c.load(path);
	CHECK(r.ok && !r.existed);
	CHECK(c.port == 8787 && c.code.size() == 10 && c.style == Style::Pixel && !c.enabled);
	CHECK(r.needs_save);
	c.port = 9000;
	c.style = Style::Gaussian;
	c.strength = 77;
	CHECK(c.save(path));
	CHECK(!std::filesystem::exists(path + ".tmp"));
	CHECK(read_text(path).find("mode") == std::string::npos); // the mode is never persisted

	ServerConfig d;
	r = d.load(path);
	CHECK(r.ok && r.existed);
	CHECK(d.port == 9000 && d.code == c.code && d.style == Style::Gaussian && d.strength == 77);

	{
		std::ofstream out(path);
		out << "{broken";
	}
	ServerConfig e;
	r = e.load(path);
	CHECK(!r.ok && r.existed && std::filesystem::exists(path + ".bad"));
	CHECK(e.port == 8787 && e.code.size() == 10 && !e.enabled);

	{
		std::ofstream out(path);
		out << R"({"port": 70000, "strength": 5000, "style": "weird"})";
	}
	ServerConfig g;
	CHECK(g.load(path).ok);
	CHECK(g.port == 8787 && g.strength == 100 && g.style == Style::Pixel && g.code.size() == 10);
}

static void test_http_server_smoke()
{
	HimitsukaoService svc;
	ApiHandler api(svc, "<html>hi</html>", "p4r7s2u9ye", [] { return int64_t(0); });
	HttpServer server(api);
	std::string error;
	CHECK(server.start("127.0.0.1", 0, &error));
	CHECK(server.running() && server.port() > 0);

	{
		httplib::Client cli("127.0.0.1", server.port());
		cli.set_connection_timeout(3);
		cli.set_read_timeout(3);
		auto page = cli.Get("/");
		CHECK(page && page->status == 200 && page->body == "<html>hi</html>");
		auto unauth = cli.Get("/api/state");
		CHECK(unauth && unauth->status == 401);
		httplib::Headers h{{"X-HimitsuKao-Code", "p4r7s2u9ye"}};
		auto state = cli.Get("/api/state", h);
		CHECK(state && state->status == 200);
		CHECK(state && state->get_header_value("Cache-Control") == "no-store");
		CHECK(state && nlohmann::json::parse(state->body)["mode"] == "normal");
		auto mode = cli.Post("/api/mode", h, R"({"mode":"off"})", "application/json");
		CHECK(mode && mode->status == 200 && svc.runtime().mode() == Mode::Off);
		auto big = cli.Post("/api/mode", h, std::string(kMaxBodyBytes + 100, 'x'), "application/json");
		CHECK(big && big->status == 413);
		auto del = cli.Delete("/api/known/zzz", h);
		CHECK(del && del->status == 404);
	}

	// A second server on a busy port reports the failure instead of crashing.
	HttpServer second(api);
	std::string error2;
	CHECK(!second.start("127.0.0.1", server.port(), &error2));
	CHECK(!error2.empty() && !second.running());

	server.stop();
	CHECK(!server.running());
	server.stop(); // idempotent
}

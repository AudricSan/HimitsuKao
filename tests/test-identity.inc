// Step 4 tests (people database, identity manager, service). Included by test-main.cpp, which provides CHECK,
// near() and the includes. Synthetic embeddings only: no real faces needed.

static Embedding synth_embedding(unsigned seed, float noise = 0.f, unsigned noise_seed = 0)
{
	std::mt19937 rng(seed);
	std::normal_distribution<float> nd(0.f, 1.f);
	Embedding e(128);
	for (auto &x : e)
		x = nd(rng);
	if (noise > 0.f) {
		std::mt19937 rn(noise_seed + 12345);
		for (auto &x : e)
			x += noise * nd(rn);
	}
	return l2_normalize(e);
}

static std::string temp_dir(const char *name)
{
	const auto p = std::filesystem::temp_directory_path() / (std::string("himitsukao_test_") + name);
	std::error_code ec;
	std::filesystem::remove_all(p, ec);
	std::filesystem::create_directories(p, ec);
	return p.string();
}

static std::string read_text(const std::string &path)
{
	std::ifstream in(path, std::ios::binary);
	std::stringstream ss;
	ss << in.rdbuf();
	return ss.str();
}

static void test_cosine()
{
	const Embedding a = synth_embedding(1);
	CHECK(near(cosine_similarity(a, a), 1.f));
	CHECK(std::fabs(cosine_similarity(a, synth_embedding(2))) < 0.4f);
	CHECK(cosine_similarity(a, Embedding(5)) == 0.f);
	CHECK(cosine_similarity(Embedding(), Embedding()) == 0.f);
}

static void test_db_add_match_remove()
{
	FaceDatabase db;
	const std::string a = db.add_person("Alice", false, synth_embedding(1), "thumbA");
	const std::string b = db.add_person("Bob", true, synth_embedding(2), "");
	CHECK(a.size() == 8 && a != b);
	CHECK(db.size() == 2);

	auto m = db.best_match(synth_embedding(1, 0.05f, 1));
	CHECK(m.has_value() && m->id == a && m->name == "Alice" && m->score > 0.9f);
	m = db.best_match(synth_embedding(2, 0.05f, 2));
	CHECK(m.has_value() && m->id == b);
	CHECK(!db.best_match(synth_embedding(3)).has_value()); // a stranger

	// The threshold is honoured.
	const Embedding noisy = synth_embedding(1, 1.0f, 7); // clearly degraded
	const float score = cosine_similarity(noisy, synth_embedding(1));
	CHECK(score > 0.2f && score < 0.95f);
	CHECK(db.best_match(noisy, score - 0.01f).has_value());
	CHECK(!db.best_match(noisy, score + 0.01f).has_value());

	// Extra embeddings: any stored embedding can match; capped at 10.
	CHECK(db.add_embedding(a, synth_embedding(10)));
	CHECK(db.best_match(synth_embedding(10, 0.05f, 3))->id == a);
	for (unsigned i = 0; i < 20; ++i)
		CHECK(db.add_embedding(a, synth_embedding(100 + i)));
	for (const auto &p : db.list())
		if (p.id == a)
			CHECK(p.embedding_count == FaceDatabase::kMaxEmbeddingsPerPerson);
	CHECK(!db.add_embedding("nope", synth_embedding(1)));

	CHECK(db.remove(b));
	CHECK(!db.remove(b));
	CHECK(!db.best_match(synth_embedding(2, 0.05f, 2)).has_value());
	CHECK(db.size() == 1);
}

static void test_db_save_load_session()
{
	const std::string dir = temp_dir("db");
	const std::string path = dir + "/known.json";
	std::string a_id;
	{
		FaceDatabase db;
		const auto r = db.load(path);
		CHECK(r.ok && !r.existed);
		a_id = db.add_person("Alice", false, synth_embedding(1), "thumbA");
		db.add_embedding(a_id, synth_embedding(11));
		db.add_person("Guest", true, synth_embedding(2), "");
		CHECK(db.save());
		CHECK(!std::filesystem::exists(path + ".tmp"));
		CHECK(db.save()); // overwriting an existing file works too
	}
	{
		FaceDatabase db;
		const auto r = db.load(path);
		CHECK(r.ok && r.existed);
		const auto people = db.list();
		CHECK(people.size() == 1); // the session person was never written
		CHECK(people[0].id == a_id && people[0].name == "Alice" && !people[0].session);
		CHECK(people[0].thumbnail_jpeg_b64 == "thumbA" && people[0].embedding_count == 2);
		CHECK(db.best_match(synth_embedding(11, 0.05f, 4))->id == a_id);
		CHECK(!db.best_match(synth_embedding(2, 0.05f, 4)).has_value());
	}
	CHECK(read_text(path).find("Guest") == std::string::npos);
}

static void test_db_corrupt_file()
{
	const std::string dir = temp_dir("corrupt");
	const std::string path = dir + "/known.json";
	{
		std::ofstream out(path);
		out << "{ this is not json";
	}
	FaceDatabase db;
	const auto r = db.load(path);
	CHECK(!r.ok && r.existed);
	CHECK(db.size() == 0);
	CHECK(std::filesystem::exists(path + ".bad"));
	CHECK(read_text(path + ".bad") == "{ this is not json");

	// Valid JSON of the wrong shape is also treated as corrupt.
	{
		std::ofstream out(path);
		out << "{\"people\": [{\"id\": \"x\"}]}";
	}
	FaceDatabase db2;
	CHECK(!db2.load(path).ok);
	CHECK(db2.size() == 0);
}

struct IdentityFixture {
	FaceDatabase db;
	EnrollmentBoard board;
	FaceTracker tracker;
	IdentityManager mgr;
	int embed_calls = 0;
	std::map<size_t, Embedding> per_detection; // what each detection "looks like"

	explicit IdentityFixture(IdentityConfig cfg = {}) : mgr(db, board, cfg) {}

	// One worker iteration: detections at `rects`, detection i has the embedding per_detection[i].
	std::vector<int> step(const std::vector<Rect> &rects, int64_t now)
	{
		auto ids = tracker.update(rects, now);
		mgr.process(
			tracker, ids, now,
			[&](size_t i) {
				++embed_calls;
				return per_detection.count(i) ? per_detection[i] : Embedding();
			},
			[](size_t) { return std::string("thumb"); });
		return ids;
	}
};

static const Rect kFace{0.4f, 0.4f, 0.1f, 0.1f};
static const Rect kFace2{0.1f, 0.1f, 0.1f, 0.1f};

static void test_recognition_sticks_and_unmasks()
{
	IdentityFixture fx;
	const std::string alice = fx.db.add_person("Alice", true, synth_embedding(1), "");
	fx.per_detection[0] = synth_embedding(1, 0.05f, 1);

	fx.step({kFace}, 0);
	CHECK(fx.tracker.tracks()[0].person_id == alice);
	CHECK(fx.tracker.tracks()[0].person_name == "Alice");
	CHECK(fx.mgr.masks(0, 300, 1.f).empty()); // known: not masked
	CHECK(fx.mgr.snapshot()[0].thumbnail_jpeg_b64 == "thumb");

	// Fluidity: a recognised track is never re-verified, even if the embeddings turn bad.
	const int calls = fx.embed_calls;
	fx.per_detection[0] = synth_embedding(99);
	for (int t = 100; t <= 5000; t += 100)
		fx.step({kFace}, t);
	CHECK(fx.embed_calls == calls);
	CHECK(fx.tracker.tracks()[0].person_id == alice);
}

static void test_unknown_recheck_interval()
{
	IdentityConfig cfg;
	cfg.recheck_ms = 1000;
	IdentityFixture fx(cfg);
	fx.per_detection[0] = synth_embedding(5); // nobody in the database
	fx.step({kFace}, 0);
	CHECK(fx.embed_calls == 1);
	CHECK(fx.mgr.masks(0, 300, 1.f).size() == 1); // unknown: masked
	fx.step({kFace}, 500);
	CHECK(fx.embed_calls == 1); // too early
	fx.step({kFace}, 1000);
	CHECK(fx.embed_calls == 2);

	// The person is added later (e.g. from the phone): the next check recognises them.
	const std::string p = fx.db.add_person("Late", true, synth_embedding(5), "");
	fx.step({kFace}, 1500);
	CHECK(fx.tracker.tracks()[0].person_id.empty());
	fx.step({kFace}, 2000);
	CHECK(fx.tracker.tracks()[0].person_id == p);
	CHECK(fx.mgr.masks(2000, 300, 1.f).empty());
}

static void test_enrollment_flow()
{
	IdentityConfig cfg;
	cfg.enroll_spacing_ms = 150;
	cfg.enroll_extra_embeddings = 4;
	IdentityFixture fx(cfg);
	const Embedding face_emb = synth_embedding(7);
	fx.per_detection[0] = face_emb;

	const auto ids = fx.step({kFace}, 0);
	CHECK(fx.mgr.masks(0, 300, 1.f).size() == 1);

	const std::string req = fx.board.new_request();
	CHECK(fx.board.get(req).state == EnrollState::Pending);
	fx.mgr.request_enroll(req, ids[0], "Carol", true);

	// First embedding: the person exists and the track is un-masked in the same iteration.
	fx.step({kFace}, 100);
	const EnrollStatus st = fx.board.get(req);
	CHECK(st.state == EnrollState::Done && !st.person_id.empty());
	CHECK(fx.tracker.tracks()[0].person_id == st.person_id);
	CHECK(fx.mgr.masks(100, 300, 1.f).empty());
	CHECK(fx.db.size() == 1);
	CHECK(fx.db.list()[0].thumbnail_jpeg_b64 == "thumb");
	CHECK(fx.db.list()[0].embedding_count == 1);

	// Extra embeddings are spaced by >= 150 ms and capped at 4.
	fx.step({kFace}, 150); // only 50 ms after the first one
	CHECK(fx.db.list()[0].embedding_count == 1);
	int64_t t = 250;
	for (int i = 0; i < 12; ++i, t += 160)
		fx.step({kFace}, t);
	CHECK(fx.db.list()[0].embedding_count == 5);
	CHECK(fx.db.best_match(face_emb).has_value());
}

static void test_enrollment_saves_persistent_only()
{
	const std::string dir = temp_dir("enroll_save");
	const std::string path = dir + "/known.json";
	IdentityConfig cfg;
	cfg.enroll_extra_embeddings = 1;
	cfg.enroll_spacing_ms = 10;
	IdentityFixture fx(cfg);
	fx.db.load(path);
	fx.per_detection[0] = synth_embedding(7);
	fx.per_detection[1] = synth_embedding(8);
	const auto ids = fx.step({kFace, kFace2}, 0);
	fx.mgr.request_enroll(fx.board.new_request(), ids[0], "Keeper", false);
	fx.mgr.request_enroll(fx.board.new_request(), ids[1], "Guest", true);
	for (int64_t t = 100; t < 600; t += 100)
		fx.step({kFace, kFace2}, t);
	CHECK(fx.db.size() == 2);

	FaceDatabase reloaded;
	reloaded.load(path);
	CHECK(reloaded.size() == 1);
	CHECK(reloaded.list()[0].name == "Keeper");
	CHECK(reloaded.list()[0].embedding_count == 2);
}

static void test_enrollment_expiry()
{
	IdentityFixture fx;
	fx.per_detection[0] = synth_embedding(7);
	const auto ids = fx.step({kFace}, 0);

	// Request for a track that does not exist: fails right away.
	const std::string ghost = fx.board.new_request();
	fx.mgr.request_enroll(ghost, 9999, "Nobody", true);
	fx.step({kFace}, 100);
	CHECK(fx.board.get(ghost).state == EnrollState::Failed);

	// The track is there but no embedding can be computed: fails after ~3 s.
	fx.per_detection.clear();
	const std::string req = fx.board.new_request();
	fx.mgr.request_enroll(req, ids[0], "Blurry", true);
	fx.step({kFace}, 200);
	CHECK(fx.board.get(req).state == EnrollState::Pending);
	fx.step({kFace}, 2000);
	CHECK(fx.board.get(req).state == EnrollState::Pending);
	fx.step({kFace}, 3300);
	CHECK(fx.board.get(req).state == EnrollState::Failed);
	CHECK(fx.db.size() == 0);
	CHECK(fx.mgr.masks(3300, 300, 1.f).size() == 1); // still masked

	// The face leaves the picture before the first embedding.
	fx.per_detection[0] = synth_embedding(7);
	const auto ids2 = fx.step({kFace}, 4000);
	const std::string req2 = fx.board.new_request();
	fx.mgr.request_enroll(req2, ids2[0], "Gone", true);
	fx.step({}, 4100); // track alive (max age) but not detected
	CHECK(fx.board.get(req2).state == EnrollState::Pending);
	fx.step({}, 5100); // track expired
	CHECK(fx.board.get(req2).state == EnrollState::Failed);
	CHECK(!fx.board.get(req2).error.empty());
	CHECK(fx.db.size() == 0);
	CHECK(fx.board.get("unknown-id").state == EnrollState::Unknown);
}

static void test_masks_skip_recognised_tracks()
{
	FaceTracker tr;
	tr.update({kFace, kFace2}, 0);
	CHECK(tr.masks(0, 300, 1.f).size() == 2);
	tr.tracks()[0].person_id = "abc";
	const auto m = tr.masks(0, 300, 1.f);
	CHECK(m.size() == 1);
	CHECK(near(m[0].x, 0.1f));
	CHECK(masks_from_tracks(tr.tracks(), 0, 300, 1.f).size() == 1);
}

static void test_service_remove_remasks_and_enroll()
{
	HimitsukaoService svc;
	IdentityManager mgr(svc.database(), svc.board(), IdentityConfig{});
	const std::string key = svc.register_instance("cam", &mgr);
	FaceTracker tracker;
	const Embedding emb = synth_embedding(7);
	auto step = [&](int64_t now) {
		auto ids = tracker.update({kFace}, now);
		mgr.process(tracker, ids, now, [&](size_t) { return emb; }, [](size_t) { return std::string("t"); });
		return ids;
	};
	const auto ids = step(0);

	auto live = svc.live_faces(0);
	CHECK(live.size() == 1 && live[0].instance_key == key && live[0].track_id == ids[0] && live[0].person_id.empty());
	CHECK(svc.live_faces(5000).empty()); // too old

	CHECK(svc.enroll("nope", ids[0], "X", true).empty());
	const std::string req = svc.enroll(key, ids[0], "Dana", true);
	CHECK(!req.empty());
	step(100);
	const auto st = svc.enroll_status(req);
	CHECK(st.state == EnrollState::Done);
	CHECK(svc.known_people().size() == 1 && svc.known_people()[0].name == "Dana");
	CHECK(mgr.masks(100, 300, 1.f).empty());
	live = svc.live_faces(100);
	CHECK(live[0].person_id == st.person_id && live[0].person_name == "Dana");

	// Removal: masked again immediately (before the worker runs again)...
	CHECK(svc.remove_person(st.person_id));
	CHECK(!svc.remove_person(st.person_id));
	CHECK(mgr.masks(100, 300, 1.f).size() == 1);
	CHECK(svc.live_faces(100)[0].person_id.empty());
	// ...and the worker's own tracker is cleaned too, so the identity does not come back.
	step(200);
	CHECK(tracker.tracks()[0].person_id.empty());
	CHECK(mgr.masks(200, 300, 1.f).size() == 1);
	step(1500);
	CHECK(tracker.tracks()[0].person_id.empty()); // the database no longer matches

	svc.unregister_instance(key);
	CHECK(svc.live_faces(0).empty());
}

static void test_thumbnail()
{
	cv::Mat frame(480, 640, CV_8UC4, cv::Scalar(30, 90, 200, 255));
	const std::string b64 = face_thumbnail_b64(frame, Rect{0.4f, 0.4f, 0.2f, 0.3f});
	CHECK(b64.size() > 100);
	CHECK(b64.compare(0, 4, "/9j/") == 0); // JPEG magic number, base64
	CHECK(face_thumbnail_b64(cv::Mat(), kFace).empty());
	const unsigned char abc[] = {'f', 'o', 'o', 'b', 'a'};
	CHECK(base64_encode(abc, 5) == "Zm9vYmE=");
	CHECK(base64_encode(abc, 4) == "Zm9vYg==");
	CHECK(base64_encode(abc, 3) == "Zm9v");
}

static void test_recognizer_smoke()
{
#ifdef HIMITSUKAO_TEST_SFACE_MODEL
	FaceRecognizer rec(HIMITSUKAO_TEST_SFACE_MODEL);
	cv::Mat crop(112, 112, CV_8UC3);
	cv::randu(crop, cv::Scalar::all(0), cv::Scalar::all(255));
	const Embedding e = rec.embed_aligned(crop);
	CHECK(e.size() == 128);
	float n = 0;
	for (float x : e)
		n += x * x;
	CHECK(near(n, 1.f, 1e-3f));
	CHECK(near(cosine_similarity(e, rec.embed_aligned(crop)), 1.f, 1e-3f)); // deterministic

	// Full path: alignCrop from a YuNet-style row (pixel coordinates of the frame passed).
	cv::Mat frame(480, 640, CV_8UC4);
	cv::randu(frame, cv::Scalar::all(0), cv::Scalar::all(255));
	Face f;
	const float row[15] = {250, 150, 120, 150, 290, 200, 350, 200, 320, 240, 295, 275, 345, 275, 0.9f};
	f.raw = cv::Mat(1, 15, CV_32F, const_cast<float *>(row)).clone();
	CHECK(rec.embed(frame, f).size() == 128);
	CHECK(rec.embed(cv::Mat(), f).empty());
	CHECK(rec.embed(frame, Face{}).empty());
#endif
}

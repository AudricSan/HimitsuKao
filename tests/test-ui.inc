// Dock / debug boxes logic (pure parts). Included by test-main.cpp after test-security.inc.

static void test_server_binding_decision()
{
	// Phone page enabled: every interface, with or without a dock.
	CHECK(decide_server_binding(true, true) == (ServerBinding{true, "0.0.0.0"}));
	CHECK(decide_server_binding(true, false) == (ServerBinding{true, "0.0.0.0"}));
	// Disabled: loopback only (the OBS dock on this PC); nothing at all if there is no dock.
	CHECK(decide_server_binding(false, true) == (ServerBinding{true, "127.0.0.1"}));
	CHECK(!decide_server_binding(false, false).run);
	CHECK(decide_server_binding(false, false).host.empty());
}

static void test_box_colors()
{
	Track unknown;
	CHECK(box_color_for(unknown, false) == BoxColor::Red);
	CHECK(box_color_for(unknown, true) == BoxColor::Orange); // enrollment pending
	Track known;
	known.person_id = "abc";
	CHECK(box_color_for(known, false) == BoxColor::Green);
	CHECK(box_color_for(known, true) == BoxColor::Green); // known wins
}

static void test_debug_boxes()
{
	IdentityFixture fx;
	fx.per_detection[0] = synth_embedding(1);
	fx.per_detection[1] = synth_embedding(2);
	const std::string alice = fx.db.add_person("Alice", true, synth_embedding(1), "");
	const auto ids = fx.step({kFace, kFace2}, 1000); // face 0 recognised, face 1 unknown

	auto boxes = fx.mgr.debug_boxes(1000, 300, 1.f);
	CHECK(boxes.size() == 2);
	for (const auto &b : boxes) {
		if (b.track_id == ids[0])
			CHECK(b.color == BoxColor::Green);
		else
			CHECK(b.color == BoxColor::Red);
	}

	// Known people are boxed too (unlike masks), and the box is the padded mask rectangle.
	CHECK(fx.mgr.masks(1000, 300, 1.f).size() == 1);
	boxes = fx.mgr.debug_boxes(1000, 300, 1.5f);
	CHECK(boxes.size() == 2);
	for (const auto &b : boxes)
		if (b.track_id == ids[1]) {
			CHECK(approx(b.rect.w, kFace2.w * 1.5f));
			CHECK(approx(b.rect.x, kFace2.x - kFace2.w * 0.25f));
		}

	// A pending enrollment turns the unknown face orange until it finishes.
	fx.per_detection[1] = Embedding(); // no usable embedding: stays pending
	const std::string req = fx.board.new_request();
	fx.mgr.request_enroll(req, ids[1], "Bob", true);
	boxes = fx.mgr.debug_boxes(1000, 300, 1.f);
	for (const auto &b : boxes)
		if (b.track_id == ids[1])
			CHECK(b.color == BoxColor::Orange);
	fx.step({kFace, kFace2}, 1100);
	fx.step({kFace, kFace2}, 4200); // timeout: the request fails
	CHECK(fx.board.get(req).state == EnrollState::Failed);
	for (const auto &b : fx.mgr.debug_boxes(4200, 300, 1.f))
		if (b.track_id == ids[1])
			CHECK(b.color == BoxColor::Red); // pending set cleaned up

	// Hold window: tracks not seen recently are not drawn.
	CHECK(fx.mgr.debug_boxes(4200 + 500, 300, 1.f).empty());
	(void)alice;
}

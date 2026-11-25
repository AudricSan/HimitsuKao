#include "face-database.h"

#include "file-util.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>

#include <nlohmann/json.hpp>

namespace himitsukao {

namespace fs = std::filesystem;
using json = nlohmann::json;

float cosine_similarity(const Embedding &a, const Embedding &b)
{
	if (a.empty() || a.size() != b.size())
		return 0.f;
	double dot = 0, na = 0, nb = 0;
	for (size_t i = 0; i < a.size(); ++i) {
		dot += static_cast<double>(a[i]) * b[i];
		na += static_cast<double>(a[i]) * a[i];
		nb += static_cast<double>(b[i]) * b[i];
	}
	if (na < 1e-12 || nb < 1e-12)
		return 0.f;
	return static_cast<float>(dot / std::sqrt(na * nb));
}

Embedding l2_normalize(Embedding v)
{
	double n = 0;
	for (float x : v)
		n += static_cast<double>(x) * x;
	n = std::sqrt(n);
	if (n < 1e-12)
		return v;
	for (float &x : v)
		x = static_cast<float>(x / n);
	return v;
}

namespace {

std::string random_hex_id()
{
	static std::mutex m;
	static std::mt19937 rng{std::random_device{}()};
	std::lock_guard<std::mutex> lock(m);
	std::ostringstream os;
	os << std::hex;
	for (int i = 0; i < 8; ++i)
		os << (rng() & 0xF);
	return os.str();
}

int64_t epoch_seconds()
{
	return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
		.count();
}

} // namespace

LoadResult FaceDatabase::load(const std::string &path)
{
	std::lock_guard<std::mutex> lock(mutex_);
	path_ = path;
	// Keep session people (none can exist before the first load in practice); replace persistent ones.
	people_.erase(std::remove_if(people_.begin(), people_.end(), [](const Person &p) { return !p.info.session; }),
		      people_.end());

	LoadResult result;
	const fs::path p = fs::u8path(path);
	std::error_code ec;
	if (!fs::exists(p, ec)) {
		result.message = "no database yet, starting empty";
		return result;
	}
	result.existed = true;

	try {
		std::ifstream in(p, std::ios::binary);
		if (!in)
			throw std::runtime_error("cannot open file");
		const json j = json::parse(in);
		std::vector<Person> loaded;
		for (const auto &jp : j.at("people")) {
			Person person;
			person.info.id = jp.at("id").get<std::string>();
			person.info.name = jp.value("name", std::string());
			person.info.session = false;
			person.info.thumbnail_jpeg_b64 = jp.value("thumbnail", std::string());
			person.info.created = jp.value("created", static_cast<int64_t>(0));
			for (const auto &je : jp.at("embeddings")) {
				Embedding e = je.get<Embedding>();
				if (!e.empty())
					person.embeddings.push_back(std::move(e));
			}
			if (person.info.id.empty() || person.embeddings.empty())
				throw std::runtime_error("invalid person entry");
			person.info.embedding_count = person.embeddings.size();
			loaded.push_back(std::move(person));
		}
		for (auto &person : loaded)
			people_.push_back(std::move(person));
		result.message = "loaded " + std::to_string(loaded.size()) + " known people";
	} catch (const std::exception &e) {
		result.ok = false;
		fs::path bad = p;
		bad += ".bad";
		std::error_code cec;
		fs::copy_file(p, bad, fs::copy_options::overwrite_existing, cec);
		result.message = std::string("database file is corrupt (") + e.what() +
				 "), starting empty; backup: " + bad.u8string() +
				 (cec ? " (backup FAILED: " + cec.message() + ")" : "");
	}
	return result;
}

bool FaceDatabase::save()
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (path_.empty())
		return true;

	json people = json::array();
	for (const auto &person : people_) {
		if (person.info.session)
			continue;
		people.push_back({{"id", person.info.id},
				  {"name", person.info.name},
				  {"created", person.info.created},
				  {"thumbnail", person.info.thumbnail_jpeg_b64},
				  {"embeddings", person.embeddings}});
	}
	const json root = {{"version", 1}, {"people", people}};

	const fs::path p = fs::u8path(path_);
	fs::path tmp = p;
	tmp += ".tmp";
	try {
		std::error_code ec;
		if (p.has_parent_path())
			fs::create_directories(p.parent_path(), ec);
		{
			std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
			if (!out)
				return false;
			out << root.dump();
			out.flush();
			if (!out)
				return false;
		}
		restrict_to_owner(tmp);
		fs::rename(tmp, p); // replaces the destination (atomic on the same volume)
		return true;
	} catch (const std::exception &) {
		std::error_code ec;
		fs::remove(tmp, ec);
		return false;
	}
}

std::string FaceDatabase::add_person(const std::string &name, bool session, const Embedding &first_embedding,
				     const std::string &thumbnail_jpeg_b64)
{
	std::lock_guard<std::mutex> lock(mutex_);
	std::string id;
	do {
		id = random_hex_id();
	} while (std::any_of(people_.begin(), people_.end(), [&](const Person &p) { return p.info.id == id; }));
	Person person;
	person.info.id = id;
	person.info.name = name;
	person.info.session = session;
	person.info.thumbnail_jpeg_b64 = thumbnail_jpeg_b64;
	person.info.created = epoch_seconds();
	person.embeddings.push_back(first_embedding);
	person.info.embedding_count = 1;
	people_.push_back(std::move(person));
	return id;
}

bool FaceDatabase::add_embedding(const std::string &id, const Embedding &embedding)
{
	std::lock_guard<std::mutex> lock(mutex_);
	for (auto &p : people_) {
		if (p.info.id != id)
			continue;
		p.embeddings.push_back(embedding);
		if (p.embeddings.size() > kMaxEmbeddingsPerPerson)
			p.embeddings.erase(p.embeddings.begin());
		p.info.embedding_count = p.embeddings.size();
		return true;
	}
	return false;
}

bool FaceDatabase::remove(const std::string &id)
{
	std::lock_guard<std::mutex> lock(mutex_);
	const auto it =
		std::remove_if(people_.begin(), people_.end(), [&](const Person &p) { return p.info.id == id; });
	if (it == people_.end())
		return false;
	people_.erase(it, people_.end());
	return true;
}

bool FaceDatabase::rename(const std::string &id, const std::string &new_name)
{
	std::lock_guard<std::mutex> lock(mutex_);
	for (auto &p : people_) {
		if (p.info.id == id) {
			p.info.name = new_name;
			return true;
		}
	}
	return false;
}

bool FaceDatabase::contains(const std::string &id) const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return std::any_of(people_.begin(), people_.end(), [&](const Person &p) { return p.info.id == id; });
}

std::vector<PersonInfo> FaceDatabase::list() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	std::vector<PersonInfo> out;
	out.reserve(people_.size());
	for (const auto &p : people_)
		out.push_back(p.info);
	return out;
}

size_t FaceDatabase::size() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return people_.size();
}

std::optional<Match> FaceDatabase::best_match(const Embedding &embedding, float threshold) const
{
	std::lock_guard<std::mutex> lock(mutex_);
	const Person *best = nullptr;
	float best_score = -2.f;
	for (const auto &p : people_) {
		for (const auto &e : p.embeddings) {
			const float s = cosine_similarity(embedding, e);
			if (s > best_score) {
				best_score = s;
				best = &p;
			}
		}
	}
	if (!best || best_score < threshold)
		return std::nullopt;
	return Match{best->info.id, best->info.name, best_score};
}

} // namespace himitsukao

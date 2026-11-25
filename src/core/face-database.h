#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace himitsukao {

using Embedding = std::vector<float>; // 128 floats, L2-normalised

// Cosine similarity of two embeddings (0 if sizes differ or a vector is null).
float cosine_similarity(const Embedding &a, const Embedding &b);

// Returns a unit-length copy (unchanged if the norm is ~0).
Embedding l2_normalize(Embedding v);

struct PersonInfo {
	std::string id;
	std::string name;
	bool session = false; // "this live only": memory only, never written to disk
	std::string thumbnail_jpeg_b64;
	int64_t created = 0; // Unix epoch, seconds
	size_t embedding_count = 0;
};

struct Match {
	std::string id;
	std::string name;
	float score = 0.f;
};

struct LoadResult {
	bool ok = true;          // false: the file was unreadable/corrupt (database is empty, backup kept)
	bool existed = false;    // a file was present
	bool needs_save = false; // the loader repaired/migrated something: the caller should save()
	std::string message;     // human readable, for the log
};

// Thread-safe database of known people. Session people live in memory only.
class FaceDatabase {
public:
	static constexpr size_t kMaxEmbeddingsPerPerson = 10;
	static constexpr float kDefaultThreshold = 0.363f;

	FaceDatabase() = default;
	FaceDatabase(const FaceDatabase &) = delete;
	FaceDatabase &operator=(const FaceDatabase &) = delete;

	// Loads `path` (and remembers it for save()). A missing file gives an empty database. A corrupt file gives an
	// empty database and is copied to `<path>.bad` (never silently overwritten).
	LoadResult load(const std::string &path);

	// Atomic write (temp file + rename) of the non-session people. No-op returning true if no path is set.
	bool save();

	// Returns the new person id (short random hex).
	std::string add_person(const std::string &name, bool session, const Embedding &first_embedding,
			       const std::string &thumbnail_jpeg_b64);
	// False if the person is unknown. Oldest embeddings are dropped beyond kMaxEmbeddingsPerPerson.
	bool add_embedding(const std::string &id, const Embedding &embedding);
	bool remove(const std::string &id);
	bool rename(const std::string &id, const std::string &new_name);
	bool contains(const std::string &id) const;

	std::vector<PersonInfo> list() const; // without embeddings
	size_t size() const;

	// Best cosine similarity over every stored embedding; returns a match only if >= threshold.
	std::optional<Match> best_match(const Embedding &embedding, float threshold = kDefaultThreshold) const;

private:
	struct Person {
		PersonInfo info;
		std::vector<Embedding> embeddings;
	};

	mutable std::mutex mutex_;
	std::vector<Person> people_;
	std::string path_;
};

} // namespace himitsukao

#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <string>

namespace himitsukao {

enum class Mode {
	Normal,   // mask unknown faces
	FacesAll, // emergency: mask every face, known people included
	FrameAll, // emergency: mask the whole frame
	Off,      // no masking
};

enum class Style { Pixel, Gaussian, Solid };

inline const char *mode_name(Mode m)
{
	switch (m) {
	case Mode::Normal:
		return "normal";
	case Mode::FacesAll:
		return "faces_all";
	case Mode::FrameAll:
		return "frame_all";
	case Mode::Off:
		return "off";
	}
	return "normal";
}

inline bool parse_mode(const std::string &s, Mode &out)
{
	for (Mode m : {Mode::Normal, Mode::FacesAll, Mode::FrameAll, Mode::Off}) {
		if (s == mode_name(m)) {
			out = m;
			return true;
		}
	}
	return false;
}

inline const char *style_name(Style s)
{
	switch (s) {
	case Style::Pixel:
		return "pixel";
	case Style::Gaussian:
		return "gaussian";
	case Style::Solid:
		return "solid";
	}
	return "pixel";
}

inline bool parse_style(const std::string &s, Style &out)
{
	for (Style v : {Style::Pixel, Style::Gaussian, Style::Solid}) {
		if (s == style_name(v)) {
			out = v;
			return true;
		}
	}
	return false;
}

constexpr int kMinStrength = 1;
constexpr int kMaxStrength = 100;

// Global masking state, readable lock-free from the render thread.
class RuntimeState {
public:
	Mode mode() const { return static_cast<Mode>(mode_.load(std::memory_order_relaxed)); }
	Style style() const { return static_cast<Style>(style_.load(std::memory_order_relaxed)); }
	int strength() const { return strength_.load(std::memory_order_relaxed); }
	// Incremented on every change (clients can detect updates).
	uint32_t version() const { return version_.load(std::memory_order_relaxed); }

	void set_mode(Mode m)
	{
		mode_.store(static_cast<int>(m), std::memory_order_relaxed);
		++version_;
	}
	void set_style(Style s, int strength)
	{
		style_.store(static_cast<int>(s), std::memory_order_relaxed);
		strength_.store(std::clamp(strength, kMinStrength, kMaxStrength), std::memory_order_relaxed);
		++version_;
	}

private:
	std::atomic<int> mode_{static_cast<int>(Mode::Normal)};
	std::atomic<int> style_{static_cast<int>(Style::Pixel)};
	std::atomic<int> strength_{50};
	std::atomic<uint32_t> version_{1};
};

} // namespace himitsukao

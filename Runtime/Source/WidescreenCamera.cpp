#include "WidescreenCamera.h"
#include "GameAddresses.h"
#include "Logger.h"
#include "Memory.h"
#include <cmath>
#include <string>

namespace KeeperForge2 {
	namespace {
		// The game's focal length in pixels (written as 20.12 fixed point), and the screen height it was tuned for.
		constexpr double OriginalFocal = 320.0;
		constexpr double OriginalHeight = 480.0;
		std::uint32_t fieldOfView = 100;
		double setFocal = 0.0;
		double setHeight = 0.0;
	}

	float WidescreenCamera::FocalLength(std::uint32_t height) {
		if (setHeight <= 0.0 || height == 0) return static_cast<float>(OriginalFocal);
		return static_cast<float>(setFocal * height / setHeight);
	}

	void WidescreenCamera::Install(std::uint32_t fieldOfViewPercent) {
		fieldOfView = fieldOfViewPercent;
		// Distance as on the original 640 pixel wide screen ("mov eax, 640" in place of loading the screen width).
		// Together with the focal length scaled by height, this gives the original framing plus extra width.
		Memory::Patch(Game::Resolve(Game::Address::PlayerCameraDistanceWidth), { 0xB8, 0x80, 0x02, 0x00, 0x00 });
	}

	// Both camera constants must match: the path camera derives its lens from the screen width divided by this value,
	// so changing both together leaves camera paths exactly as they were.
	void WidescreenCamera::OnScreenMode(std::int32_t height) {
		if (height <= 0) { return; }
		const double focal = OriginalFocal * (height / OriginalHeight) * (100.0 / fieldOfView);
		const auto fixed = static_cast<std::int32_t>(std::lround(focal * 4096.0));
		Memory::Write<std::int32_t>(Game::Resolve(Game::Address::PlayerCameraFocal) + 4, fixed);
		Memory::Write<std::int32_t>(Game::Resolve(Game::Address::PathCameraFocal) + 4, fixed);
		setFocal = focal;
		setHeight = height;
		Logger::Debug("Widescreen: camera focal length " + std::to_string(focal) + " for a " + std::to_string(height) + " pixel tall screen.");
	}
}

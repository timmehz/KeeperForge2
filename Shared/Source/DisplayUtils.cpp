#include "DisplayUtils.h"
#include <charconv>
#include <Windows.h>
namespace KeeperForge2 {
	bool DisplayUtils::ParseResolution(const std::string& text, std::uint32_t& width, std::uint32_t& height) {
		const auto separator = text.find('x');
		if (separator == std::string::npos) { return false; }
		const char* begin = text.data();
		const char* end = text.data() + text.size();
		const auto widthResult = std::from_chars(begin, begin + separator, width);
		const auto heightResult = std::from_chars(begin + separator + 1, end, height);
		return widthResult.ec == std::errc{} && widthResult.ptr == begin + separator && heightResult.ec == std::errc{} && heightResult.ptr == end;
	}
	bool DisplayUtils::GetDesktopResolution(std::uint32_t& width, std::uint32_t& height) {
		DEVMODEW mode{};
		mode.dmSize = sizeof(mode);
		if (!EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &mode)) { return false; }
		width = mode.dmPelsWidth;
		height = mode.dmPelsHeight;
		return true;
	}
}

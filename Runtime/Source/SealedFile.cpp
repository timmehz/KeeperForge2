#include "SealedFile.h"
#include "Logger.h"
#include <fstream>
#include <iterator>
#include <mutex>
#include <vector>
#include <Windows.h>
#include <dpapi.h>

namespace KeeperForge2::SealedFile {
	namespace {
		std::mutex writeMutex;
		std::string Entropy(std::string_view purpose) { return "KeeperForge2 " + std::string(purpose) + " v1"; }
	}

	bool Write(const std::filesystem::path& path, std::string_view purpose, const std::string& text) {
		const std::string extra = Entropy(purpose);
		DATA_BLOB plain{ static_cast<DWORD>(text.size()), reinterpret_cast<BYTE*>(const_cast<char*>(text.data())) };
		DATA_BLOB entropy{ static_cast<DWORD>(extra.size()), reinterpret_cast<BYTE*>(const_cast<char*>(extra.data())) };
		DATA_BLOB sealed{};
		const std::wstring description = L"KeeperForge2 " + std::wstring(purpose.begin(), purpose.end());
		if (!CryptProtectData(&plain, description.c_str(), &entropy, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &sealed)) {
			Logger::Warning("Could not seal " + path.filename().string() + " for saving (error " + std::to_string(GetLastError()) + "); it is not saved.");
			return false;
		}
		const std::vector<char> bytes(sealed.pbData, sealed.pbData + sealed.cbData);
		LocalFree(sealed.pbData);
		std::lock_guard lock(writeMutex);
		auto temporary = path;
		temporary += ".tmp";
		{
			std::ofstream file(temporary, std::ios::out | std::ios::binary | std::ios::trunc);
			if (file) file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
			if (!file) {
				Logger::Warning("Could not write " + temporary.string() + ".");
				return false;
			}
		}
		std::error_code error;
		std::filesystem::rename(temporary, path, error);
		if (error) {
			Logger::Warning("Could not save " + path.string() + " (" + error.message() + ").");
			return false;
		}
		return true;
	}

	std::optional<std::string> Read(const std::filesystem::path& path, std::string_view purpose, bool& missing) {
		std::ifstream file(path, std::ios::in | std::ios::binary);
		missing = !file;
		if (!file) return std::nullopt;
		std::vector<BYTE> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
		file.close();
		const std::string extra = Entropy(purpose);
		DATA_BLOB sealed{ static_cast<DWORD>(bytes.size()), bytes.data() };
		DATA_BLOB entropy{ static_cast<DWORD>(extra.size()), reinterpret_cast<BYTE*>(const_cast<char*>(extra.data())) };
		DATA_BLOB plain{};
		if (!CryptUnprotectData(&sealed, nullptr, &entropy, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &plain)) return std::nullopt;
		std::string text(reinterpret_cast<const char*>(plain.pbData), plain.cbData);
		SecureZeroMemory(plain.pbData, plain.cbData);
		LocalFree(plain.pbData);
		return text;
	}
}

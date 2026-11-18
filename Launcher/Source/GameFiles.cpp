#include "GameFiles.h"
#include "Logger.h"
#include <algorithm>
#include <array>
#include <fstream>
#include <memory>
#include <string_view>
#include <system_error>
#include <vector>
#include <Windows.h>
#include <bcrypt.h>

namespace fs = std::filesystem;
namespace KeeperForge2 {
	namespace {
		// A copy of a file known to be original: its size in bytes and SHA-256 checksum (lowercase hex).
		struct KnownCopy {
			std::uintmax_t size;
			std::string_view sha256;
		};
		struct RequiredFile {
			std::string_view name;
			// Checked by size and checksum when true; only by name otherwise (KeeperForge2's own files change with
			// every build).
			bool original;
			// Known original copies. While a game file has none listed, the launcher logs its size and checksum
			// instead of checking them, so they can be added here.
			std::vector<KnownCopy> copies;
		};
		const std::vector<RequiredFile>& RequiredFiles() {
			// The Steam release (v1.70).
			static const std::vector<RequiredFile> files = {
				{ "Aweman32.dll", true, { { 35328, "184450e8d2b588d0659e0f57455b0106b26ffe77ffab15563819f335a97d12db" } } },
				{ "ddraw.dll", true, { { 2824704, "3657600ed4dc51fe0c49d1b44a0a9f98dcebf0d24aee76520d9b8c17478cb590" } } },
				{ "dinput.dll", true, { { 271360, "ddae8f4fdfde0387020e9a75b5367e2a0d82fbe3b4f02f468927c5ca99636dde" } } },
				{ "DKII.EXE", true, { { 2987568, "b5e4e2f6b76335e2a2e7f43bb6903bfc6f4170eabf04598955b00f512a47b449" } } },
				{ "DKII-DX.exe", true, { { 2987568, "a5957e8c52b6dee80ddf6cc1d3ea5079aaaad4d11b40e0f067d34ff4157ce9bb" } } },
				{ "patch.dll", true, { { 150016, "0022e3aaf419f22c3287fb454e9e671ee249f1eefc652465bec96ecfd38f00c4" } } },
				{ "QMIXER.DLL", true, { { 198656, "460292fe61a75996e8035f8a65a765733738293d0d391336be292aa74c128698" } } },
				{ "SFMAN32.DLL", true, { { 48640, "91db92aa170a5b8e757ce2bd36156b558cb5b4f66383a0f2a1a05c9bb7e039b1" } } },
				{ "WEANETR.dll", true, { { 142848, "fafd6e306432d245c3ce575535367ae43e62224b1177121edfa25c31a8f18b73" } } },
				{ "KeeperForge2.dll", false, {} },
			};
			return files;
		}
		std::string SizeText(std::uintmax_t size) { return std::to_string(size) + " bytes"; }
		struct AlgorithmCloser { void operator()(BCRYPT_ALG_HANDLE algorithm) const { BCryptCloseAlgorithmProvider(algorithm, 0); } };
		struct HashDestroyer { void operator()(BCRYPT_HASH_HANDLE hash) const { BCryptDestroyHash(hash); } };
	}

	std::optional<std::string> GameFiles::Sha256(const fs::path& file) {
		std::ifstream input(file, std::ios::binary);
		if (!input.is_open()) { return std::nullopt; }
		// The hash is closed before the algorithm it was made from. Each out_ptr fills its handle only when its whole
		// statement ends, so these two calls cannot be joined into one expression.
		std::unique_ptr<void, AlgorithmCloser> algorithm;
		std::unique_ptr<void, HashDestroyer> hash;
		if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(std::out_ptr(algorithm), BCRYPT_SHA256_ALGORITHM, nullptr, 0))) { return std::nullopt; }
		if (!BCRYPT_SUCCESS(BCryptCreateHash(algorithm.get(), std::out_ptr(hash), nullptr, 0, nullptr, 0, 0))) { return std::nullopt; }
		std::array<UCHAR, 32> digest{};
		std::vector<char> buffer(1 << 16);
		while (input) {
			input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
			const auto read = input.gcount();
			if (read > 0 && !BCRYPT_SUCCESS(BCryptHashData(hash.get(), reinterpret_cast<PUCHAR>(buffer.data()), static_cast<ULONG>(read), 0))) { return std::nullopt; }
		}
		if (!input.eof() || !BCRYPT_SUCCESS(BCryptFinishHash(hash.get(), digest.data(), static_cast<ULONG>(digest.size()), 0))) { return std::nullopt; }
		constexpr char digits[] = "0123456789abcdef";
		std::string text;
		for (const auto byte : digest) {
			text += digits[byte >> 4];
			text += digits[byte & 0xF];
		}
		return text;
	}

	void GameFiles::Verify(const fs::path& gameDirectory) {
		std::vector<std::string> problems;
		for (const auto& required : RequiredFiles()) {
			const std::string name(required.name);
			const fs::path file = gameDirectory / name;
			std::error_code error;
			if (!fs::is_regular_file(file, error)) {
				problems.push_back(name + ": missing.");
				continue;
			}
			if (!required.original) { continue; }
			const auto size = fs::file_size(file, error);
			const auto checksum = error ? std::nullopt : Sha256(file);
			if (!checksum) {
				problems.push_back(name + ": could not be read.");
				continue;
			}
			if (required.copies.empty()) {
				Logger::Warning("Game file " + name + " has no known original size and checksum yet; this copy is " + SizeText(size) + ", SHA-256 " + *checksum + ".");
				continue;
			}
			const bool sizeKnown = std::ranges::contains(required.copies, size, &KnownCopy::size);
			if (std::ranges::any_of(required.copies, [&](const KnownCopy& copy) { return copy.size == size && copy.sha256 == *checksum; })) {
				Logger::Debug("Game file " + name + " is original (" + SizeText(size) + ", SHA-256 " + *checksum + ").");
				continue;
			}
			std::string problem = name + ": not an original copy (" + (sizeKnown ? "checksum" : "size and checksum") + " differ).\n   This copy: " + SizeText(size) + ", SHA-256 " + *checksum;
			for (const auto& copy : required.copies) { problem += "\n   Original: " + SizeText(copy.size) + ", SHA-256 " + std::string(copy.sha256); }
			problems.push_back(problem);
		}
		if (problems.empty()) {
			Logger::Info("Required files have been found and checked.");
			return;
		}
		std::string message = "Some Dungeon Keeper 2 files are missing or not original:";
		for (const auto& problem : problems) { message += "\n - " + problem; }
		Logger::Error(message);
	}
}

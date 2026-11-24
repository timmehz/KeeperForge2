#include "CustomCampaign.h"
#include "FrameRate.h"
#include "GameAddresses.h"
#include "HookManager.h"
#include "Logger.h"
#include "Memory.h"
#include "Paths.h"
#include "SkirmishSwapKeeper.h"
#include "StringUtils.h"
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <fstream>
#include <map>
#include <string>
#include <system_error>
#include <utility>
#include <vector>
#include <Windows.h>
#include <wincodec.h>

namespace fs = std::filesystem;

// Our own design (Flame's custom campaign button is unfinished). See docs/game/custom-campaign.md.
namespace KeeperForge2 {
	namespace {
		constexpr std::int32_t MapListWindowId = 34;
		constexpr std::int32_t ActionOpenMapList = 0x58;
		constexpr std::int32_t OpenMapListArgument = 0x22;
		constexpr std::int32_t ActionMapListTick = 0x4E;
		constexpr std::int32_t ActionMapListCross = 0x4F;
		constexpr std::int32_t ActionStartSkirmish = 0x5A;
		constexpr std::int32_t ActionLeaveSkirmish = 0x18;
		constexpr std::int32_t BriefingWindowId = 7;
		constexpr std::int32_t ActionBriefingTick = 0x0C;
		constexpr std::int32_t ActionBriefingCross = 0x0A;
		// The briefing text table's ids (0x553AC0), by where the game shows them [unverified meanings, from the layout]:
		// 0 and 1 the level's name (the briefing's title shows "0" - 1), 2 the Main Objective box (and first in the in-game
		// objectives), 3, 4 and 5 the Sub Objective box's lines (tested: the briefing window draws them one per line).
		constexpr std::size_t BriefingTexts = 6;
		constexpr std::size_t ChosenMapChars = 0x40;
		// The most levels a campaign may have (1.kwd to 999.kwd).
		constexpr std::uint32_t MaxCampaignLevels = 999;
		constexpr std::int32_t StatusWon = 2;
		constexpr char CampaignFile[] = "campaign.yml";
		constexpr char ProgressFile[] = "progress.yml";

		// A level's texts from campaign.yml ("<level>.title", ".briefing", ".objective", ".subobjectives" split by '|'), and
		// its briefing picture ("<level>.image", else the campaign's "image"; a file in the campaign's folder).
		struct LevelTexts {
			std::wstring title;
			std::wstring briefing;
			std::wstring objective;
			std::vector<std::wstring> subObjectives;
			std::wstring image;
			bool Any() const { return !title.empty() || !briefing.empty() || !objective.empty() || !subObjectives.empty() || !image.empty(); }
		};
		struct Campaign {
			fs::path folder;
			std::wstring name;
			std::vector<std::wstring> levels;
			std::vector<LevelTexts> texts;
			std::vector<bool> won;
			std::size_t WonCount() const { return static_cast<std::size_t>(std::count(won.begin(), won.end(), true)); }
			// The first level not won yet, or levels.size() when all are.
			std::size_t NextLevel() const {
				for (std::size_t i = 0; i < won.size(); ++i) if (!won[i]) return i;
				return levels.size();
			}
		};
		std::vector<Campaign> campaigns;

		// Where the player is in the menus.
		bool listMode = false;        // the Custom Campaign button was pressed: the map list shows campaigns
		bool listWanted = false;      // show our list where the skirmish screen would show
		bool listInstalled = false;   // our rows are in the map list now
		std::uint32_t gameList = 0;   // the game's own rows, put back after
		std::int32_t gameListCount = -1;
		std::uint32_t ourList = 0;
		// Our list shows the campaigns, or the chosen campaign's levels (picked from it; none is forced).
		enum class ListKind { Campaigns, Levels };
		ListKind listKind = ListKind::Campaigns;
		ListKind wantedKind = ListKind::Campaigns;
		std::int32_t wantedRow = -1;
		// The campaign whose levels are listed, and the one being played.
		std::size_t chosenCampaign = 0;
		bool armedForGame = false;
		bool inSession = false;
		bool redirectOn = false;
		std::size_t activeCampaign = 0;
		std::size_t activeLevel = 0;
		bool resultSeen = false;
		bool levelWon = false;
		// The game in the results screen (built on the way back to the menus) was one of ours.
		bool resultsForOurs = false;
		// The briefing window shows the level about to start.
		bool briefingShown = false;

		std::wstring Wide(const std::string& text) {
			if (text.empty()) return {};
			const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
			std::wstring wide(static_cast<std::size_t>(size), L'\0');
			MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), size);
			return wide;
		}
		std::string Narrow(const std::wstring& text) {
			if (text.empty()) return {};
			const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
			std::string narrow(static_cast<std::size_t>(size), '\0');
			WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), narrow.data(), size, nullptr, nullptr);
			return narrow;
		}
		std::string Trim(std::string text) {
			while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) text.pop_back();
			std::size_t start = 0;
			while (start < text.size() && (text[start] == ' ' || text[start] == '\t')) ++start;
			text = text.substr(start);
			if (text.size() >= 2 && (text.front() == '"' || text.front() == '\'') && text.back() == text.front()) text = text.substr(1, text.size() - 2);
			return text;
		}
		// "key: value" lines; '#' starts a comment line. Read only: a campaign's own files are never written.
		std::map<std::string, std::string> ReadKeyValues(const fs::path& path) {
			std::map<std::string, std::string> values;
			std::ifstream file(path, std::ios::binary);
			std::string line;
			bool first = true;
			while (std::getline(file, line)) {
				if (first && line.starts_with("\xEF\xBB\xBF")) line = line.substr(3);
				first = false;
				const auto trimmed = Trim(line);
				if (trimmed.empty() || trimmed.front() == '#') continue;
				const auto colon = trimmed.find(':');
				if (colon == std::string::npos) continue;
				values[StringUtils::ToLower(Trim(trimmed.substr(0, colon)))] = Trim(trimmed.substr(colon + 1));
			}
			return values;
		}
		std::vector<std::string> SplitList(const std::string& text) {
			std::vector<std::string> items;
			std::size_t start = 0;
			while (start <= text.size()) {
				const auto comma = text.find(',', start);
				const auto item = Trim(text.substr(start, comma == std::string::npos ? std::string::npos : comma - start));
				if (!item.empty()) items.push_back(item);
				if (comma == std::string::npos) break;
				start = comma + 1;
			}
			return items;
		}

		void SaveProgress(const Campaign& campaign) {
			std::string won;
			for (std::size_t i = 0; i < campaign.levels.size(); ++i) {
				if (!campaign.won[i]) continue;
				if (!won.empty()) won += ", ";
				won += Narrow(campaign.levels[i]);
			}
			std::ofstream file(campaign.folder / ProgressFile, std::ios::binary | std::ios::trunc);
			file << "# Written by KeeperForge2: the levels of this campaign won so far.\n" << "won: " << won << "\n";
			if (!file) Logger::Warning("Custom campaign: could not write " + (campaign.folder / ProgressFile).string() + ".");
		}

		void LoadCampaigns() {
			campaigns.clear();
			std::error_code error;
			const auto root = Paths::Campaigns();
			if (!fs::is_directory(root, error)) return;
			std::vector<fs::path> folders;
			for (const auto& entry : fs::directory_iterator(root, fs::directory_options::skip_permission_denied, error)) {
				if (entry.is_directory(error)) folders.push_back(entry.path());
			}
			std::ranges::sort(folders);
			for (const auto& folder : folders) {
				const auto file = folder / CampaignFile;
				if (!fs::is_regular_file(file, error)) continue;
				const auto values = ReadKeyValues(file);
				Campaign campaign;
				campaign.folder = folder;
				const auto name = values.contains("name") ? values.at("name") : folder.filename().string();
				campaign.name = Wide(name);
				bool usable = true;
				// The levels are the folder's 1.kwd, 2.kwd, 3.kwd ... in that order, up to the first number missing. Each
				// .kwd lists its .kld parts by their own names; those are read from this folder too (LevelFileFor).
				for (std::uint32_t number = 1; number <= MaxCampaignLevels; ++number) {
					const auto wide = std::to_wstring(number);
					if (!fs::is_regular_file(folder / (wide + L".kwd"), error)) break;
					campaign.levels.push_back(wide);
				}
				if (values.contains("levels")) {
					Logger::Warning("Custom campaign \"" + name + "\": campaign.yml's levels line is not used; the levels are the folder's 1.kwd, 2.kwd, ... in order.");
				}
				// A numbered level after a gap would never be played.
				for (const auto& entry : fs::directory_iterator(folder, fs::directory_options::skip_permission_denied, error)) {
					if (StringUtils::ToLower(entry.path().extension().string()) != ".kwd") continue;
					const auto stem = entry.path().stem().wstring();
					if (stem.empty() || !std::ranges::all_of(stem, [](wchar_t c) { return c >= L'0' && c <= L'9'; })) continue;
					const auto number = std::wcstoul(stem.c_str(), nullptr, 10);
					if (number > campaign.levels.size()) {
						Logger::Warning("Custom campaign \"" + name + "\": " + Narrow(stem) + ".kwd is left out, as " + std::to_string(campaign.levels.size() + 1)
							+ ".kwd is missing (levels are numbered from 1 with no gaps).");
					}
				}
				for (const auto& level : campaign.levels) {
					const auto key = StringUtils::ToLower(Narrow(level)) + ".";
					const auto text = [&values, &key](const char* part) {
						const auto found = values.find(key + part);
						return found != values.end() ? Wide(found->second) : std::wstring();
					};
					LevelTexts texts{ text("title"), text("briefing"), text("objective"), {}, text("image") };
					if (texts.image.empty() && values.contains("image")) texts.image = Wide(values.at("image"));
					std::wstring subs = text("subobjectives");
					for (std::size_t start = 0; start <= subs.size();) {
						const auto bar = subs.find(L'|', start);
						auto item = subs.substr(start, bar == std::wstring::npos ? std::wstring::npos : bar - start);
						while (!item.empty() && item.front() == L' ') item.erase(item.begin());
						while (!item.empty() && item.back() == L' ') item.pop_back();
						if (!item.empty()) texts.subObjectives.push_back(item);
						if (bar == std::wstring::npos) break;
						start = bar + 1;
					}
					campaign.texts.push_back(std::move(texts));
				}
				if (campaign.levels.empty()) {
					Logger::Warning("Custom campaign \"" + name + "\": the folder has no 1.kwd (levels are 1.kwd, 2.kwd, ... in the campaign's folder).");
					usable = false;
				}
				if (!usable) continue;
				campaign.won.assign(campaign.levels.size(), false);
				const auto progress = ReadKeyValues(folder / ProgressFile);
				if (progress.contains("won")) {
					for (const auto& level : SplitList(progress.at("won"))) {
						const auto wide = StringUtils::ToLower(level);
						for (std::size_t i = 0; i < campaign.levels.size(); ++i) {
							if (StringUtils::ToLower(Narrow(campaign.levels[i])) == wide) campaign.won[i] = true;
						}
					}
				}
				campaigns.push_back(std::move(campaign));
			}
			Logger::Info("Custom campaign: " + std::to_string(campaigns.size()) + " campaign(s) in " + root.string() + ".");
		}

		// ---- The map list showing campaigns.
		using MenuActionFn = std::int32_t(__cdecl*)(std::int32_t argument, std::int32_t action, std::uint8_t* frontEnd);
		MenuActionFn originalMenuAction = nullptr;
		using ListClickFn = std::int32_t(__cdecl*)(std::int32_t row, std::int32_t second, std::uint8_t* frontEnd);
		ListClickFn originalListClick = nullptr;

		std::uint8_t* FrontEnd() { return reinterpret_cast<std::uint8_t*>(Game::Resolve(Game::Address::FrontEndComponent)); }
		// The menu window shown: the first in the front end's window list with its shown flag set, as the game's own lookup
		// (0x5322D0) does; -1 when none is.
		std::int32_t Window(const std::uint8_t* frontEnd) {
			auto* window = *reinterpret_cast<const std::uint8_t* const*>(frontEnd + Game::Offset::FrontEndWindowList);
			for (int guard = 0; window != nullptr && guard < 256; ++guard) {
				if (*reinterpret_cast<const std::int32_t*>(window + Game::Offset::MenuWindowShown) != 0)
					return *reinterpret_cast<const std::int32_t*>(window + Game::Offset::MenuWindowId);
				window = *reinterpret_cast<const std::uint8_t* const*>(window + Game::Offset::MenuWindowNext);
			}
			return -1;
		}
		// Whether that menu window is shown (looked up by its id, as the game's 0x52C540 does).
		bool Shown(const std::uint8_t* frontEnd, std::int32_t id) {
			auto* window = *reinterpret_cast<const std::uint8_t* const*>(frontEnd + Game::Offset::FrontEndWindowList);
			for (int guard = 0; window != nullptr && guard < 256; ++guard) {
				if (*reinterpret_cast<const std::int32_t*>(window + Game::Offset::MenuWindowId) == id)
					return *reinterpret_cast<const std::int32_t*>(window + Game::Offset::MenuWindowShown) != 0;
				window = *reinterpret_cast<const std::uint8_t* const*>(window + Game::Offset::MenuWindowNext);
			}
			return false;
		}

		// A level's name in the level list and its details: "N. <title>", or "Level N" without a title.
		std::wstring LevelName(const Campaign& campaign, std::size_t level) {
			const auto number = std::to_wstring(level + 1);
			const auto& title = level < campaign.texts.size() ? campaign.texts[level].title : std::wstring();
			return title.empty() ? L"Level " + number : number + L". " + title;
		}
		bool LevelsListed() { return listKind == ListKind::Levels && chosenCampaign < campaigns.size(); }
		std::size_t RowCount() { return LevelsListed() ? campaigns[chosenCampaign].levels.size() : campaigns.size(); }
		std::wstring RowName(std::size_t row) {
			if (LevelsListed()) return LevelName(campaigns[chosenCampaign], row);
			return row < campaigns.size() ? campaigns[row].name : std::wstring();
		}
		// One row per campaign (name, "won / levels" in the players column), or per level of the chosen campaign (its name,
		// "Won" once won), as the game's own rows: then an empty size column and a fourth field; each field ended by 0, the
		// list by 0xFFFF.
		std::uint32_t MakeRows() {
			std::wstring text;
			const auto addRow = [&text](const std::wstring& name, const std::wstring& second, const std::wstring& fourth) {
				for (const auto* field : { &name, &second, &fourth }) {
					text += *field;
					text += L'\0';
					// The size column, between the second and fourth fields, stays empty.
					if (field == &second) text += L'\0';
				}
			};
			if (LevelsListed()) {
				const auto& campaign = campaigns[chosenCampaign];
				for (std::size_t level = 0; level < campaign.levels.size(); ++level) {
					addRow(LevelName(campaign, level), campaign.won[level] ? L"Won" : L"", campaign.levels[level]);
				}
			}
			else {
				for (const auto& campaign : campaigns) {
					addRow(campaign.name, std::to_wstring(campaign.WonCount()) + L" / " + std::to_wstring(campaign.levels.size()), campaign.folder.filename().wstring());
				}
			}
			text += static_cast<wchar_t>(0xFFFF);
			// From the game's allocator, as the front end frees the list itself when it ends.
			auto* memory = reinterpret_cast<wchar_t*>(reinterpret_cast<void*(__cdecl*)(std::uint32_t)>(Game::Resolve(Game::Address::GameAlloc))(
				static_cast<std::uint32_t>(text.size() * sizeof(wchar_t))));
			if (memory == nullptr) return 0;
			std::memcpy(memory, text.data(), text.size() * sizeof(wchar_t));
			return reinterpret_cast<std::uint32_t>(memory);
		}
		// ---- The list's title, "Custom Campaigns" in place of "Map Select". Window titles are pictures the front end makes
		// once (0x536850); ours is made the same way into pictures of our own, and swapped with the map list's title (title 9)
		// while the list shows campaigns.
		// The map list title widget's text field (button config + 0x40) is 9: title 9 (text 2800). (Its + 0x44 is 6, not
		// the picture's number: title 6 is Load Game's.)
		constexpr std::size_t MapListTitleIndex = 9;
		constexpr std::size_t ImageRecordBytes = 0x28;
		constexpr std::size_t TitleSizeBytes = 0x10;
		constexpr std::uint32_t TitleWideFontLimit = 0x276;
		constexpr std::uint32_t TitleFont = 9;
		constexpr std::uint32_t TitleSmallFont = 8;
#pragma pack(push, 1)
		struct TitleBakeSettings {
			const char* text;
			void* image;
			void* image2;
			const std::int32_t* area;
			std::int32_t* size;
			const std::uint8_t* colour;
			std::uint32_t zero;
			std::uint8_t three;
			std::uint32_t font;
			std::uint32_t two;
		};
#pragma pack(pop)
		static_assert(sizeof(TitleBakeSettings) == 0x25);
		struct Title {
			alignas(4) std::uint8_t image[ImageRecordBytes]{};
			alignas(4) std::uint8_t image2[ImageRecordBytes]{};
			std::int32_t size[4]{};
		};
		Title ourTitle;
		bool titleMade = false;
		bool titleFailed = false;
		bool titleSwapped = false;

		// The thiscall functions, called as fastcall with an unused EDX.
		using ImageCreateFn = void*(__fastcall*)(void* record, void* unused, const std::int32_t* widthHeight, void* format, std::int32_t, std::int32_t);
		using ImageAllocateFn = void(__fastcall*)(void* record, void* unused, std::int32_t* result);
		using TitleBakeFn = void(__fastcall*)(std::uint8_t* frontEnd, void* unused, TitleBakeSettings* settings);
		using ConvertTitleFn = std::int32_t(__cdecl*)(const wchar_t* text, char* out, std::int32_t size);

		bool MakePictureSized(std::uint8_t* record, std::int32_t width, std::int32_t height) {
			const std::int32_t widthHeight[2] = { width, height };
			alignas(4) std::uint8_t made[ImageRecordBytes]{};
			auto* result = reinterpret_cast<ImageCreateFn>(Game::Resolve(Game::Address::ImageCreate))(
				made, nullptr, widthHeight, reinterpret_cast<void*>(Game::Resolve(Game::Address::ImagePixelFormat)), 0, 0);
			std::memcpy(record, result != nullptr ? result : made, ImageRecordBytes);
			std::int32_t allocated = -1;
			reinterpret_cast<ImageAllocateFn>(Game::Resolve(Game::Address::ImageAllocate))(record, nullptr, &allocated);
			auto* pixels = *reinterpret_cast<std::uint8_t**>(record + 0x18);
			if (allocated != 0 || pixels == nullptr) return false;
			std::memset(pixels, 0, static_cast<std::size_t>(*reinterpret_cast<std::uint32_t*>(record + 0x20)) * *reinterpret_cast<std::uint32_t*>(record + 0x24));
			return true;
		}
		// A title picture's size (as the front end makes its titles).
		bool MakePicture(std::uint8_t* record) { return MakePictureSized(record, 0x280, 0x46); }
		// As the front end makes its titles (0x536850), with our text.
		void MakeTitle(std::uint8_t* frontEnd) {
			if (titleMade || titleFailed) return;
			titleFailed = true;
			if (!MakePicture(ourTitle.image) || !MakePicture(ourTitle.image2)) {
				Logger::Warning("Custom campaign: could not make the list's title picture; it keeps the game's.");
				return;
			}
			static char text[0x100]{};
			reinterpret_cast<ConvertTitleFn>(Game::Resolve(Game::Address::ConvertText))(L"Custom Campaigns", text, static_cast<std::int32_t>(sizeof(text)));
			static constexpr std::int32_t area[4] = { 0, 0, 0x280, 0x46 };
			static constexpr std::uint8_t colour[5] = { 0x33, 0xB0, 0xFF, 0xFF, 0x00 };
			TitleBakeSettings settings{ text, ourTitle.image, ourTitle.image2, area, ourTitle.size, colour, 0, 3, TitleFont, 2 };
			const auto bake = reinterpret_cast<TitleBakeFn>(Game::Resolve(Game::Address::TitleBake));
			bake(frontEnd, nullptr, &settings);
			if (static_cast<std::uint32_t>(ourTitle.size[2] - ourTitle.size[0]) >= TitleWideFontLimit) {
				settings.font = TitleSmallFont;
				bake(frontEnd, nullptr, &settings);
			}
			titleFailed = false;
			titleMade = true;
		}
		// ---- The list's column headings: text widgets that ask for their text by id each frame (0x5323B0). While the list
		// shows campaigns their text ids are ours ("Campaign", "Won"), and the size column's is 0, which draws nothing.
		constexpr std::int32_t MapListNameHeading = 470;
		constexpr std::int32_t MapListPlayersHeading = 471;
		constexpr std::int32_t MapListSizeHeading = 472;
		constexpr std::int32_t CampaignHeadingText = 60002;
		constexpr std::int32_t WonHeadingText = 60003;
		struct HeadingChange { std::uint8_t* widget; std::int32_t gameText; };
		std::vector<HeadingChange> headingChanges;
		std::uint8_t* FindWidget(std::uint8_t* frontEnd, std::int32_t windowId, std::int32_t widgetId) {
			auto* window = *reinterpret_cast<std::uint8_t**>(frontEnd + Game::Offset::FrontEndWindowList);
			for (int guard = 0; window != nullptr && guard < 256; ++guard) {
				if (*reinterpret_cast<const std::int32_t*>(window + Game::Offset::MenuWindowId) == windowId) break;
				window = *reinterpret_cast<std::uint8_t**>(window + Game::Offset::MenuWindowNext);
			}
			if (window == nullptr) return nullptr;
			auto* widget = *reinterpret_cast<std::uint8_t**>(window + Game::Offset::MenuWindowWidgets);
			for (int guard = 0; widget != nullptr && guard < 256; ++guard) {
				if (*reinterpret_cast<const std::int32_t*>(widget + Game::Offset::MenuWidgetId) == widgetId) return widget;
				widget = *reinterpret_cast<std::uint8_t**>(widget + Game::Offset::MenuWidgetNext);
			}
			return nullptr;
		}
		void SetHeadings(std::uint8_t* frontEnd) {
			if (!headingChanges.empty()) return;
			for (const auto [widgetId, text] : { std::pair{ MapListNameHeading, CampaignHeadingText }, std::pair{ MapListPlayersHeading, WonHeadingText },
				std::pair{ MapListSizeHeading, 0 } }) {
				auto* widget = FindWidget(frontEnd, MapListWindowId, widgetId);
				if (widget == nullptr) continue;
				auto& field = *reinterpret_cast<std::int32_t*>(widget + Game::Offset::MenuWidgetText);
				headingChanges.push_back({ widget, field });
				field = text;
			}
		}
		// The size column's green is cropped off the background and the row highlight while the list shows campaigns, by
		// narrowing the pictures' widths: the list is 1240 wide in the menu's layout, its third column starting at 880. Both
		// are drawn from the same left edge, so the highlight is cut at the background's new width.
		constexpr std::uintptr_t PictureWidth = 0x1C;
		constexpr std::int32_t ListLayoutWidth = 1240;
		constexpr std::int32_t ThirdColumnStart = 880;
		std::int32_t backgroundWidth = 0;
		std::int32_t highlightWidth = 0;
		std::int32_t& Width(std::uintptr_t picture) { return *reinterpret_cast<std::int32_t*>(Game::Resolve(picture) + PictureWidth); }
		void CropBackground() {
			if (backgroundWidth != 0) return;
			auto& width = Width(Game::Address::MapListBackground);
			if (width <= 0) return;
			backgroundWidth = width;
			width = static_cast<std::int32_t>(static_cast<std::int64_t>(width) * ThirdColumnStart / ListLayoutWidth);
			auto& highlight = Width(Game::Address::MapListRowHighlight);
			highlightWidth = highlight;
			highlight = (std::min)(highlight, width);
		}
		void RestoreBackground() {
			if (backgroundWidth == 0) return;
			Width(Game::Address::MapListBackground) = backgroundWidth;
			Width(Game::Address::MapListRowHighlight) = highlightWidth;
			backgroundWidth = 0;
			highlightWidth = 0;
		}
		void RestoreHeadings() {
			for (const auto& change : headingChanges) *reinterpret_cast<std::int32_t*>(change.widget + Game::Offset::MenuWidgetText) = change.gameText;
			headingChanges.clear();
		}

		// Swaps our title with the map list's (the same call puts the game's back).
		void SwapTitle(std::uint8_t* frontEnd) {
			auto swap = [](std::uint8_t* a, std::uint8_t* b, std::size_t bytes) {
				std::uint8_t temporary[ImageRecordBytes];
				std::memcpy(temporary, a, bytes);
				std::memcpy(a, b, bytes);
				std::memcpy(b, temporary, bytes);
			};
			swap(frontEnd + Game::Offset::FrontEndTitleImages + MapListTitleIndex * ImageRecordBytes, ourTitle.image, ImageRecordBytes);
			swap(frontEnd + Game::Offset::FrontEndTitleImages2 + MapListTitleIndex * ImageRecordBytes, ourTitle.image2, ImageRecordBytes);
			swap(frontEnd + Game::Offset::FrontEndTitleSizes + MapListTitleIndex * TitleSizeBytes, reinterpret_cast<std::uint8_t*>(ourTitle.size), TitleSizeBytes);
			titleSwapped = !titleSwapped;
		}

		void InstallList() {
			ourList = MakeRows();
			if (ourList == 0) return;
			auto& list = *reinterpret_cast<std::uint32_t*>(Game::Resolve(Game::Address::SkirmishMapList));
			auto& count = *reinterpret_cast<std::int32_t*>(Game::Resolve(Game::Address::SkirmishMapListCount));
			gameList = list;
			gameListCount = count;
			list = ourList;
			count = -1;
			*reinterpret_cast<std::int32_t*>(Game::Resolve(Game::Address::MapListChosenRow)) = -1;
			listInstalled = true;
			MakeTitle(FrontEnd());
			if (titleMade && !titleSwapped) SwapTitle(FrontEnd());
			SetHeadings(FrontEnd());
			CropBackground();
		}
		// Shows the other kind of rows in the list already shown (the campaigns, or the chosen campaign's levels).
		void SwitchRows(ListKind kind, std::int32_t row) {
			if (!listInstalled) return;
			listKind = kind;
			const auto rows = MakeRows();
			if (rows == 0) return;
			auto& list = *reinterpret_cast<std::uint32_t*>(Game::Resolve(Game::Address::SkirmishMapList));
			auto& count = *reinterpret_cast<std::int32_t*>(Game::Resolve(Game::Address::SkirmishMapListCount));
			if (list == ourList) list = rows;
			count = -1;
			reinterpret_cast<void(__cdecl*)(void*)>(Game::Resolve(Game::Address::GameFree))(reinterpret_cast<void*>(ourList));
			ourList = rows;
			*reinterpret_cast<std::int32_t*>(Game::Resolve(Game::Address::MapListChosenRow)) = row;
		}
		void RestoreList() {
			if (!listInstalled) return;
			listInstalled = false;
			if (titleSwapped) SwapTitle(FrontEnd());
			RestoreHeadings();
			RestoreBackground();
			auto& list = *reinterpret_cast<std::uint32_t*>(Game::Resolve(Game::Address::SkirmishMapList));
			auto& count = *reinterpret_cast<std::int32_t*>(Game::Resolve(Game::Address::SkirmishMapListCount));
			if (list == ourList) {
				list = gameList;
				count = gameListCount;
			}
			reinterpret_cast<void(__cdecl*)(void*)>(Game::Resolve(Game::Address::GameFree))(reinterpret_cast<void*>(ourList));
			ourList = 0;
			*reinterpret_cast<std::int32_t*>(Game::Resolve(Game::Address::MapListChosenRow)) = -1;
		}

		// The chosen campaign's details: only the name banner. While our rows show, the details draw (0x540600) gets a list
		// in which the chosen row is its name with the list's end (0xFFFF) right after it, no 0 between: the draw looks for
		// the second field by walking to the name's end and only gives up when that walk stops on 0xFFFF (a 0 then 0xFFFF
		// still drew "Max Players :", tested). The name's text conversion (0x553B20) stops at the 0 after it, and writes a
		// 0 byte for 0xFFFF (no game character for it), so the banner shows the name alone.
		using DetailsFn = std::int32_t(__cdecl*)(std::uint8_t* widget, std::uint8_t* frontEnd);
		DetailsFn originalDetails = nullptr;
		std::wstring detailsList;
		std::int32_t __cdecl DetailsDetour(std::uint8_t* widget, std::uint8_t* frontEnd) {
			const auto row = *reinterpret_cast<const std::int32_t*>(Game::Resolve(Game::Address::MapListChosenRow));
			if (!listInstalled || row < 0 || static_cast<std::size_t>(row) >= RowCount()) return originalDetails(widget, frontEnd);
			// The rows before it as 4 empty fields each, as the draw counts fields to find the row.
			detailsList.assign(static_cast<std::size_t>(row) * 4, L'\0');
			detailsList += RowName(static_cast<std::size_t>(row));
			detailsList += static_cast<wchar_t>(0xFFFF);
			detailsList += L'\0';
			auto& list = *reinterpret_cast<std::uint32_t*>(Game::Resolve(Game::Address::SkirmishMapList));
			const auto shown = list;
			list = reinterpret_cast<std::uint32_t>(detailsList.data());
			const auto result = originalDetails(widget, frontEnd);
			list = shown;
			return result;
		}

		std::int32_t __cdecl ListClickDetour(std::int32_t row, std::int32_t second, std::uint8_t* frontEnd) {
			// Our rows are not maps: only the choice is kept, no map preview is loaded.
			if (!listInstalled) return originalListClick(row, second, frontEnd);
			*reinterpret_cast<std::int32_t*>(Game::Resolve(Game::Address::MapListChosenRow)) = row;
			return 0;
		}
		using ChangeWindowFn = void(__cdecl*)(std::int32_t show, std::int32_t window, std::uint8_t* frontEnd);
		// Hides or shows a menu window (changeGui, 0x5391F0). The game's buttons do this after their action (0x528610);
		// an action run on its own switches no windows, so each step here hides and shows them itself.
		void ShowWindow(std::uint8_t* frontEnd, std::int32_t window, bool show) {
			reinterpret_cast<ChangeWindowFn>(Game::Resolve(Game::Address::FrontEndChangeWindow))(show ? 1 : 0, window, frontEnd);
		}
		void LeaveLevelChoice() {
			redirectOn = false;
			armedForGame = false;
			briefingShown = false;
		}

		// The campaign list: the skirmish map list with our rows, opened as the skirmish screen's map button does (its
		// action 0x58 with 0x22, then the skirmish screen hidden and the map list shown), so the skirmish screen never shows.
		void OpenCampaignList(std::uint8_t* frontEnd) {
			listWanted = false;
			listMode = true;
			LoadCampaigns();
			listKind = wantedKind == ListKind::Levels && chosenCampaign < campaigns.size() ? ListKind::Levels : ListKind::Campaigns;
			ShowWindow(frontEnd, Game::SkirmishWindowId, false);
			originalMenuAction(OpenMapListArgument, ActionOpenMapList, frontEnd);
			// The map list's action turns the chosen map's preview on; campaigns have none.
			frontEnd[Game::Offset::FrontEndPreviewShown] = 0;
			ShowWindow(frontEnd, MapListWindowId, true);
			if (!listInstalled) InstallList();
			if (listInstalled && wantedRow >= 0 && static_cast<std::size_t>(wantedRow) < RowCount())
				*reinterpret_cast<std::int32_t*>(Game::Resolve(Game::Address::MapListChosenRow)) = wantedRow;
			Logger::Debug(std::string("Custom campaign: the ") + (LevelsListed() ? "level" : "campaign") + " list is shown.");
		}
		// Back to the Single Player menu, as the skirmish screen's Back does (its window hidden, then action 0x18, which
		// flies the camera back and shows the Single Player menu).
		void LeaveToSinglePlayer(std::uint8_t* frontEnd) {
			listMode = false;
			ShowWindow(frontEnd, Game::SkirmishWindowId, false);
			originalMenuAction(0, ActionLeaveSkirmish, frontEnd);
		}

		// ---- The briefing picture: drawn on the left of the briefing window (empty in the game's own), by a widget of ours
		// added to the window's list. The file is read with Windows' own image decoder (WIC: PNG, JPEG, BMP and others),
		// then made into a game picture record at the widget's size on screen, keeping the file's shape.
		constexpr std::int32_t BriefingImageButtonId = 9102;
		constexpr std::int32_t BriefingTemplateButtonId = 165;
		// In the window's 2560 x 1920 layout: left of the text boxes (from x 764), from the main objective's heading down
		// to the bottom of the sub objective box.
		constexpr std::int16_t BriefingImageX = 64;
		constexpr std::int16_t BriefingImageY = 436;
		constexpr std::int16_t BriefingImageWidth = 640;
		constexpr std::int16_t BriefingImageHeight = 1148;
		constexpr std::size_t MaxBriefingButtons = 32;
		constexpr std::uint32_t MaxImageSide = 4096;
		std::vector<std::uint8_t> briefingButtons;

		struct LoadedImage {
			std::uint32_t width = 0;
			std::uint32_t height = 0;
			std::vector<std::uint8_t> bgra;
		};
		LoadedImage briefingImage;
		fs::path briefingImagePath;
		template<typename T> void ReleaseCom(T*& object) {
			if (object != nullptr) {
				object->Release();
				object = nullptr;
			}
		}
		bool ReadImage(const fs::path& path, LoadedImage& image) {
			const HRESULT comResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
			// RPC_E_CHANGED_MODE: COM is already set up on this thread in another mode, which is fine to use.
			const bool uninitialize = SUCCEEDED(comResult);
			IWICImagingFactory* factory = nullptr;
			IWICBitmapDecoder* decoder = nullptr;
			IWICBitmapFrameDecode* frame = nullptr;
			IWICFormatConverter* converter = nullptr;
			UINT width = 0;
			UINT height = 0;
			bool ok = SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)))
				&& SUCCEEDED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder))
				&& SUCCEEDED(decoder->GetFrame(0, &frame))
				&& SUCCEEDED(factory->CreateFormatConverter(&converter))
				&& SUCCEEDED(converter->Initialize(frame, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom))
				&& SUCCEEDED(converter->GetSize(&width, &height))
				&& width > 0 && height > 0 && width <= MaxImageSide && height <= MaxImageSide;
			if (ok) {
				image.width = width;
				image.height = height;
				image.bgra.assign(static_cast<std::size_t>(width) * height * 4, 0);
				ok = SUCCEEDED(converter->CopyPixels(nullptr, width * 4, static_cast<UINT>(image.bgra.size()), image.bgra.data()));
			}
			ReleaseCom(converter);
			ReleaseCom(frame);
			ReleaseCom(decoder);
			ReleaseCom(factory);
			if (uninitialize) CoUninitialize();
			if (!ok) image = {};
			return ok;
		}
		// Reads the level's picture when its briefing opens (kept while the same file is wanted).
		void LoadBriefingImage(const Campaign& campaign, const LevelTexts& texts) {
			if (texts.image.empty()) {
				briefingImage = {};
				briefingImagePath.clear();
				return;
			}
			const auto path = campaign.folder / texts.image;
			if (path == briefingImagePath && briefingImage.width != 0) return;
			briefingImagePath = path;
			if (ReadImage(path, briefingImage)) {
				Logger::Debug("Custom campaign: briefing picture " + path.filename().string() + ", " + std::to_string(briefingImage.width) + "x"
					+ std::to_string(briefingImage.height) + ".");
			}
			else {
				Logger::Warning("Custom campaign \"" + Narrow(campaign.name) + "\": could not read the briefing picture " + path.string()
					+ " (PNG, JPEG or BMP, at most 4096 pixels a side).");
			}
		}

		// The picture record drawn (the game's pixel format, made at the size it is drawn).
		alignas(4) std::uint8_t briefingPicture[ImageRecordBytes]{};
		bool briefingPictureMade = false;
		std::int32_t briefingPictureWidth = 0;
		std::int32_t briefingPictureHeight = 0;
		const std::uint8_t* briefingPictureFrom = nullptr;
		void FreeBriefingPicture() {
			if (!briefingPictureMade) return;
			reinterpret_cast<void(__fastcall*)(void*)>(Game::Resolve(Game::Address::ImageFree))(briefingPicture);
			briefingPictureMade = false;
			briefingPictureFrom = nullptr;
		}
		// A colour channel (0-255) placed into a pixel by its mask.
		std::uint32_t Channel(std::uint32_t value, std::uint32_t mask) {
			if (mask == 0) return 0;
			std::uint32_t shift = 0;
			while (((mask >> shift) & 1) == 0) ++shift;
			const std::uint32_t maximum = mask >> shift;
			return ((value * maximum + 127) / 255) << shift;
		}
		bool MakeBriefingPicture(std::int32_t width, std::int32_t height) {
			FreeBriefingPicture();
			if (!MakePictureSized(briefingPicture, width, height)) return false;
			briefingPictureMade = true;
			briefingPictureWidth = width;
			briefingPictureHeight = height;
			briefingPictureFrom = briefingImage.bgra.data();
			// The record starts with the format: red, green, blue and alpha masks, then bits per pixel.
			const auto* record = reinterpret_cast<const std::uint32_t*>(briefingPicture);
			const std::uint32_t red = record[0];
			const std::uint32_t green = record[1];
			const std::uint32_t blue = record[2];
			const std::uint32_t alpha = record[3];
			const std::uint32_t bytesPerPixel = (record[4] + 7) / 8;
			auto* pixels = *reinterpret_cast<std::uint8_t**>(briefingPicture + 0x18);
			const auto pitch = *reinterpret_cast<const std::uint32_t*>(briefingPicture + 0x24);
			if (bytesPerPixel == 0 || bytesPerPixel > 4) return true;
			// Each picture pixel is the average of the file's pixels under it; see-through parts are laid on black.
			for (std::int32_t y = 0; y < height; ++y) {
				const auto top = static_cast<std::uint32_t>(static_cast<std::uint64_t>(y) * briefingImage.height / height);
				const auto bottom = (std::max)(top + 1, static_cast<std::uint32_t>(static_cast<std::uint64_t>(y + 1) * briefingImage.height / height));
				for (std::int32_t x = 0; x < width; ++x) {
					const auto left = static_cast<std::uint32_t>(static_cast<std::uint64_t>(x) * briefingImage.width / width);
					const auto right = (std::max)(left + 1, static_cast<std::uint32_t>(static_cast<std::uint64_t>(x + 1) * briefingImage.width / width));
					std::uint32_t sum[3]{};
					std::uint32_t count = 0;
					for (std::uint32_t sy = top; sy < bottom && sy < briefingImage.height; ++sy) {
						for (std::uint32_t sx = left; sx < right && sx < briefingImage.width; ++sx) {
							const auto* source = &briefingImage.bgra[(static_cast<std::size_t>(sy) * briefingImage.width + sx) * 4];
							for (int c = 0; c < 3; ++c) sum[c] += static_cast<std::uint32_t>(source[c]) * source[3] / 255;
							++count;
						}
					}
					if (count == 0) count = 1;
					const std::uint32_t value = Channel(sum[2] / count, red) | Channel(sum[1] / count, green) | Channel(sum[0] / count, blue) | alpha;
					std::memcpy(pixels + static_cast<std::size_t>(y) * pitch + static_cast<std::size_t>(x) * bytesPerPixel, &value, bytesPerPixel);
				}
			}
			return true;
		}

		using PictureDrawFn = void(__fastcall*)(void* drawer, void* unused, std::int32_t* out, void* frame, std::int32_t x, std::int32_t y, void* picture,
			const std::int32_t* area, std::int32_t mode);
		using ButtonRectFn = std::int32_t*(__fastcall*)(void* button, void* unused, std::int32_t* box);
		using BoxToScreenFn = std::int32_t*(__fastcall*)(void* manager, void* unused, std::int32_t* box, const std::int32_t* layoutBox);
		char __cdecl RenderBriefingImage(std::uint8_t* button, std::uint8_t* frontEnd) {
			if (!briefingShown || briefingImage.width == 0) return 0;
			std::int32_t layoutBox[4]{};
			std::int32_t box[4]{};
			reinterpret_cast<ButtonRectFn>(Game::Resolve(Game::Address::GuiButtonRect))(button, nullptr, layoutBox);
			reinterpret_cast<BoxToScreenFn>(Game::Resolve(Game::Address::GuiBoxToScreen))(frontEnd + Game::Offset::MenuGuiManager, nullptr, box, layoutBox);
			const std::int32_t boxWidth = box[2] - box[0];
			const std::int32_t boxHeight = box[3] - box[1];
			if (boxWidth <= 0 || boxHeight <= 0) return 0;
			// Fitted inside the box, keeping the file's shape, centred across and at the top.
			std::int32_t width = boxWidth;
			std::int32_t height = static_cast<std::int32_t>(static_cast<std::int64_t>(briefingImage.height) * width / briefingImage.width);
			if (height > boxHeight) {
				height = boxHeight;
				width = static_cast<std::int32_t>(static_cast<std::int64_t>(briefingImage.width) * height / briefingImage.height);
			}
			if (width <= 0 || height <= 0) return 0;
			if (!briefingPictureMade || briefingPictureWidth != width || briefingPictureHeight != height || briefingPictureFrom != briefingImage.bgra.data()) {
				if (!MakeBriefingPicture(width, height)) return 0;
			}
			std::int32_t out[4]{};
			reinterpret_cast<PictureDrawFn>(Game::Resolve(Game::Address::PictureDraw))(frontEnd + Game::Offset::FrontEndPictureDrawer, nullptr, out,
				frontEnd + Game::Offset::FrontEndFrame, box[0] + (boxWidth - width) / 2, box[1], briefingPicture, nullptr, 0);
			return 0;
		}
		// The briefing window's list with our picture widget before its end, copied from one of its boxes.
		bool AddBriefingImageWidget() {
			const auto config = Game::Resolve(Game::Address::BriefingWindowConfig);
			const auto buttons = Memory::Read<std::uint32_t>(config + Game::Offset::WindowConfigButtons);
			if (Memory::Read<std::uint8_t>(config) != BriefingWindowId || buttons != Game::Resolve(Game::Address::BriefingButtons)) return false;
			const auto* list = reinterpret_cast<const std::uint8_t*>(buttons);
			std::size_t count = 0;
			const std::uint8_t* boxEntry = nullptr;
			for (; count < MaxBriefingButtons; ++count) {
				std::int32_t id = 0;
				std::memcpy(&id, list + count * Game::ButtonConfigSize + Game::Offset::ButtonConfigId, sizeof(id));
				if (id == -1) break;
				if (id == BriefingTemplateButtonId) boxEntry = list + count * Game::ButtonConfigSize;
			}
			if (count == MaxBriefingButtons || boxEntry == nullptr) return false;
			briefingButtons.assign((count + 2) * Game::ButtonConfigSize, 0);
			std::memcpy(briefingButtons.data(), list, count * Game::ButtonConfigSize);
			auto* ours = briefingButtons.data() + count * Game::ButtonConfigSize;
			std::memcpy(ours, boxEntry, Game::ButtonConfigSize);
			const auto store = [ours](std::uintptr_t offset, auto value) { std::memcpy(ours + offset, &value, sizeof(value)); };
			store(Game::Offset::ButtonConfigId, BriefingImageButtonId);
			store(Game::Offset::ButtonConfigX, BriefingImageX);
			store(Game::Offset::ButtonConfigY, BriefingImageY);
			store(Game::Offset::ButtonConfigWidth, BriefingImageWidth);
			store(Game::Offset::ButtonConfigHeight, BriefingImageHeight);
			store(Game::Offset::ButtonConfigRender, reinterpret_cast<std::uint32_t>(&RenderBriefingImage));
			store(Game::Offset::ButtonConfigTextId, std::int32_t{ 0 });
			store(Game::Offset::ButtonConfigTextIndex, std::int32_t{ 0 });
			std::memcpy(ours + Game::ButtonConfigSize, list + count * Game::ButtonConfigSize, Game::ButtonConfigSize);
			Memory::Write<std::uint32_t>(config + Game::Offset::WindowConfigButtons, reinterpret_cast<std::uint32_t>(briefingButtons.data()));
			return true;
		}

		void StartLevel(std::uint8_t* frontEnd, std::size_t level);

		// The map list's tick and cross buttons hide their window after their action (button config + 0x12, the widget's
		// + 0x5D, read by the click 0x528610 after the action). When ours keep the list up (switching its rows), that flag
		// is cleared for the click and put back on the next frame.
		constexpr std::int32_t MapListCrossButton = 467;
		constexpr std::int32_t MapListTickButton = 529;
		std::uint8_t* heldButton = nullptr;
		std::int32_t heldHideFlag = 0;
		void KeepListShown(std::uint8_t* frontEnd, std::int32_t action) {
			auto* button = FindWidget(frontEnd, MapListWindowId, action == ActionMapListTick ? MapListTickButton : MapListCrossButton);
			if (button == nullptr || heldButton != nullptr) return;
			auto& flag = *reinterpret_cast<std::int32_t*>(button + Game::Offset::MenuWidgetHidesWindow);
			heldButton = button;
			heldHideFlag = flag;
			flag = 0;
		}
		void PutBackHideFlag() {
			if (heldButton == nullptr) return;
			*reinterpret_cast<std::int32_t*>(heldButton + Game::Offset::MenuWidgetHidesWindow) = heldHideFlag;
			heldButton = nullptr;
		}

		std::int32_t __cdecl MenuActionDetour(std::int32_t argument, std::int32_t action, std::uint8_t* frontEnd) {
			// Our briefing: its tick starts the level as the skirmish screen's start does, its cross goes back to the campaign
			// list (the cross's button hides the briefing itself after this).
			if (briefingShown && Shown(frontEnd, BriefingWindowId) && (action == ActionBriefingTick || action == ActionBriefingCross)) {
				briefingShown = false;
				if (action == ActionBriefingTick) {
					ShowWindow(frontEnd, BriefingWindowId, false);
					return originalMenuAction(0, ActionStartSkirmish, frontEnd);
				}
				// Back to the campaign's level list, on the same level.
				LeaveLevelChoice();
				wantedKind = ListKind::Levels;
				wantedRow = static_cast<std::int32_t>(activeLevel);
				OpenCampaignList(frontEnd);
				ShowWindow(frontEnd, BriefingWindowId, false);
				return 0;
			}
			if (!listInstalled || !Shown(frontEnd, MapListWindowId) || (action != ActionMapListTick && action != ActionMapListCross))
				return originalMenuAction(argument, action, frontEnd);
			const auto row = *reinterpret_cast<const std::int32_t*>(Game::Resolve(Game::Address::MapListChosenRow));
			const bool chosen = row >= 0 && static_cast<std::size_t>(row) < RowCount();
			// A tick with nothing chosen does nothing; the list stays.
			if (action == ActionMapListTick && !chosen) {
				KeepListShown(frontEnd, action);
				return 0;
			}
			// A campaign's tick lists its levels (the first not won chosen); a level list's cross goes back to the campaigns.
			if (!LevelsListed() && action == ActionMapListTick) {
				chosenCampaign = static_cast<std::size_t>(row);
				const auto next = campaigns[chosenCampaign].NextLevel();
				KeepListShown(frontEnd, action);
				SwitchRows(ListKind::Levels, static_cast<std::int32_t>(next < campaigns[chosenCampaign].levels.size() ? next : 0));
				return 0;
			}
			if (LevelsListed() && action == ActionMapListCross) {
				KeepListShown(frontEnd, action);
				SwitchRows(ListKind::Campaigns, static_cast<std::int32_t>(chosenCampaign));
				return 0;
			}
			const bool startLevel = LevelsListed() && action == ActionMapListTick;
			RestoreList();
			listMode = false;
			// The game's own tick and cross hide the map list and show the skirmish screen again; ours go on from there at once.
			const auto result = originalMenuAction(argument, action, frontEnd);
			if (startLevel) StartLevel(frontEnd, static_cast<std::size_t>(row));
			else LeaveToSinglePlayer(frontEnd);
			return result;
		}

		void StartLevel(std::uint8_t* frontEnd, std::size_t level) {
			if (chosenCampaign >= campaigns.size() || level >= campaigns[chosenCampaign].levels.size()) return;
			auto& campaign = campaigns[chosenCampaign];
			auto* chosen = reinterpret_cast<wchar_t*>(frontEnd + Game::Offset::FrontEndChosenMap);
			std::memset(chosen, 0, ChosenMapChars * sizeof(wchar_t));
			std::wmemcpy(chosen, campaign.levels[level].c_str(), campaign.levels[level].size());
			activeCampaign = chosenCampaign;
			activeLevel = level;
			redirectOn = true;
			armedForGame = true;
			Logger::Info("Custom campaign \"" + Narrow(campaign.name) + "\": starting level " + std::to_string(level + 1) + " of "
				+ std::to_string(campaign.levels.size()) + ", " + Narrow(campaign.levels[level]) + ".");
			ShowWindow(frontEnd, Game::SkirmishWindowId, false);
			// With texts in campaign.yml the game's briefing window shows them first; its tick starts the level.
			if (campaign.texts[level].Any()) {
				briefingShown = true;
				// The full briefing (title with both names, the sub objective box): the two short-briefing flags the front end
				// copies from the last level it read the details of (0x535C30; set for secret levels, tested: after playing
				// Secret1 the briefing lost its second title and its sub objectives) are cleared, as before any level is read.
				*reinterpret_cast<std::uint32_t*>(frontEnd + Game::Offset::FrontEndShortBriefing) = 0;
				*reinterpret_cast<std::uint32_t*>(frontEnd + Game::Offset::FrontEndShortBriefing2) = 0;
				LoadBriefingImage(campaign, campaign.texts[level]);
				ShowWindow(frontEnd, BriefingWindowId, true);
				return;
			}
			originalMenuAction(0, ActionStartSkirmish, frontEnd);
		}

		// Where the skirmish screen would show: our list instead.
		void OnSkirmishScreen(std::uint8_t* frontEnd) {
			if (listWanted) OpenCampaignList(frontEnd);
		}
		// The step that shows the skirmish screen (0x538AC0): it still sets the screen up (action 0x15), then ours takes its
		// place in the same call, before a frame is drawn. SkirmishSwapKeeper hooks it already and calls
		// CustomCampaign::AfterSkirmishSetup; this hook is only for when it does not.
		using SkirmishStepFn = void(__cdecl*)(std::uint8_t* frontEnd);
		SkirmishStepFn originalSkirmishStep = nullptr;
		void __cdecl SkirmishStepDetour(std::uint8_t* frontEnd) {
			originalSkirmishStep(frontEnd);
			if (listWanted) OnSkirmishScreen(frontEnd);
		}

		// Every frame: puts the game's map list back once ours is gone, and takes over the skirmish screen if it was shown
		// some other way.
		void AfterPresent() {
			const auto playerInterface = Game::Resolve(Game::Address::PlayerInterface);
			if (Memory::Read<void*>(playerInterface + Game::Offset::InterfaceSession) != nullptr) return;
			auto* frontEnd = FrontEnd();
			PutBackHideFlag();
			if (listInstalled && !Shown(frontEnd, MapListWindowId)) {
				RestoreList();
				listMode = false;
			}
			if (listWanted && Shown(frontEnd, Game::SkirmishWindowId)) {
				Logger::Debug("Custom campaign: the skirmish screen showed without its step (front end window "
					+ std::to_string(Window(frontEnd)) + "); taking it over.");
				OnSkirmishScreen(frontEnd);
			}
		}

		// ---- The results screen after a skirmish: its table (0x53C520, fastcall (front end)) has a row per player of the
		// chosen skirmish map's record (front end + 0x657C records of 0x890 bytes, the chosen one + 0x6608, its players
		// + 0x80). Our levels are no skirmish map, so that count was wrong, the table was not made and drawing it crashed
		// (0x53D682, tested after winning a level). After one of ours, a valid record is chosen and given 1 player (you) for
		// the build, then both are put back.
		constexpr std::uintptr_t MapRecordBytes = 0x890;
		constexpr std::uintptr_t MapRecordPlayers = 0x80;
		using ResultsFn = void(__fastcall*)(std::uint8_t* frontEnd);
		ResultsFn originalResults = nullptr;
		void __fastcall ResultsDetour(std::uint8_t* frontEnd) {
			auto* records = *reinterpret_cast<std::uint8_t**>(frontEnd + Game::Offset::FrontEndMapRecords);
			const auto recordCount = *reinterpret_cast<const std::int32_t*>(frontEnd + Game::Offset::FrontEndMapRecordCount);
			if (!resultsForOurs || records == nullptr || recordCount <= 0) return originalResults(frontEnd);
			auto& chosen = *reinterpret_cast<std::int32_t*>(frontEnd + Game::Offset::FrontEndChosenMapRecord);
			const auto chosenBefore = chosen;
			if (chosen < 0 || chosen >= recordCount) chosen = 0;
			auto& players = *reinterpret_cast<std::uint32_t*>(records + static_cast<std::size_t>(chosen) * MapRecordBytes + MapRecordPlayers);
			const auto playersBefore = players;
			players = 1;
			originalResults(frontEnd);
			players = playersBefore;
			chosen = chosenBefore;
		}

		// ---- The level's result.
		using SetStatusFn = void(__fastcall*)(std::uint8_t* player, void* unused, std::int32_t result);
		SetStatusFn originalSetStatus = nullptr;
		// With the campaign's ending (CoopCampaign's EndingMode), a win runs 0x4BEDC0's branch: the level's flags at
		// 0x6F2957 with bit 3 or 4 set (the game's secret levels) unlock a secret level of the real campaign (0x5615F0,
		// 0x561750), else Horny comes for the Portal Gem (0x4BF410). For our levels both bits are off for the call, so the
		// real campaign is never touched and Horny always comes.
		constexpr std::uint8_t SecretUnlockFlags = 0x18;
		void __fastcall SetStatusDetour(std::uint8_t* player, void* unused, std::int32_t result) {
			auto& levelFlags = *reinterpret_cast<std::uint8_t*>(Game::Resolve(Game::Address::LevelEndingFlags));
			const auto flagsBefore = levelFlags;
			if (inSession) levelFlags = static_cast<std::uint8_t>(levelFlags & ~SecretUnlockFlags);
			originalSetStatus(player, unused, result);
			if (inSession) levelFlags = flagsBefore;
			if (!inSession || resultSeen || player == nullptr) return;
			const auto* world = Memory::Read<std::uint8_t*>(Game::Resolve(Game::Address::WorldPointer));
			if (world == nullptr) return;
			const auto local = *reinterpret_cast<const std::uint16_t*>(world + Game::Offset::WorldLocalPlayerTag);
			if (*reinterpret_cast<const std::uint16_t*>(player + Game::Offset::PlayerTag) != local) return;
			resultSeen = true;
			levelWon = result == StatusWon;
			if (activeCampaign >= campaigns.size()) return;
			auto& campaign = campaigns[activeCampaign];
			Logger::Info("Custom campaign \"" + Narrow(campaign.name) + "\": level " + Narrow(campaign.levels[activeLevel]) + (levelWon ? " won." : " lost."));
			if (levelWon) {
				campaign.won[activeLevel] = true;
				SaveProgress(campaign);
			}
		}

		// ---- The briefing and objective texts. The game reads them from the level's briefing table by id (0x553AC0, cdecl
		// (id), game text); for a custom campaign's level, while its briefing shows or it is played, campaign.yml's are given.
		using BriefingTextFn = const char*(__cdecl*)(std::int32_t id);
		BriefingTextFn originalBriefingText = nullptr;
		using ConvertTextFn = std::int32_t(__cdecl*)(const wchar_t* text, char* out, std::int32_t size);
		constexpr std::size_t BriefingTextBytes = 0x1000;
		std::vector<std::vector<char>> briefingTexts;
		std::size_t textsCampaign = static_cast<std::size_t>(-1);
		std::size_t textsLevel = static_cast<std::size_t>(-1);
		std::wstring BriefingWide(const LevelTexts& texts, std::size_t id) {
			switch (id) {
				// The title reads "Level N" - <title>.
				case 0: return L"Level " + std::to_wstring(activeLevel + 1);
				case 1: return texts.title.empty() ? campaigns[activeCampaign].levels[activeLevel] : texts.title;
				// The Main Objective box: the objective, then the briefing (the window has no box of its own for it).
				case 2:
					if (texts.briefing.empty()) return texts.objective;
					if (texts.objective.empty()) return texts.briefing;
					return texts.objective + L" " + texts.briefing;
				case 3: return texts.subObjectives.size() > 0 ? texts.subObjectives[0] : std::wstring();
				case 4: return texts.subObjectives.size() > 1 ? texts.subObjectives[1] : std::wstring();
				case 5: {
					// The rest of the sub objectives share the last line.
					std::wstring rest;
					for (std::size_t i = 2; i < texts.subObjectives.size(); ++i) rest += (rest.empty() ? L"" : L"  ") + texts.subObjectives[i];
					return rest;
				}
				default: return {};
			}
		}
		const char* __cdecl BriefingTextDetour(std::int32_t id) {
			if (!(briefingShown || inSession) || activeCampaign >= campaigns.size() || id < 0 || static_cast<std::size_t>(id) >= BriefingTexts)
				return originalBriefingText(id);
			const auto& campaign = campaigns[activeCampaign];
			if (activeLevel >= campaign.texts.size() || !campaign.texts[activeLevel].Any()) return originalBriefingText(id);
			// Made in the game's own text form once per level, and kept, as callers hold the pointer while they draw.
			if (textsCampaign != activeCampaign || textsLevel != activeLevel) {
				textsCampaign = activeCampaign;
				textsLevel = activeLevel;
				briefingTexts.assign(BriefingTexts, std::vector<char>(BriefingTextBytes, '\0'));
				const auto convert = reinterpret_cast<ConvertTextFn>(Game::Resolve(Game::Address::ConvertText));
				for (std::size_t i = 0; i < BriefingTexts; ++i) {
					const auto wide = BriefingWide(campaign.texts[activeLevel], i);
					if (!wide.empty()) convert(wide.c_str(), briefingTexts[i].data(), static_cast<std::int32_t>(BriefingTextBytes));
				}
			}
			static bool logged[BriefingTexts]{};
			if (!logged[id]) {
				logged[id] = true;
				Logger::Debug("Custom campaign: the game asked for briefing text " + std::to_string(id) + " (first time).");
			}
			return briefingTexts[static_cast<std::size_t>(id)].data();
		}

		bool Hook(std::uintptr_t target, const std::vector<std::uint8_t>& expected, void* detour, void** original) {
			if (!Memory::Matches(target, expected)) return false;
			if (HookManager::CreateHook(target, detour, original) && HookManager::EnableHook(target)) return true;
			HookManager::RemoveHook(target);
			return false;
		}
		bool installed = false;
	}

	void CustomCampaign::OnMenuButton() {
		if (!installed) return;
		listMode = true;
		listWanted = true;
		wantedKind = ListKind::Campaigns;
		wantedRow = -1;
	}

	bool CustomCampaign::InSession() { return inSession; }

	void CustomCampaign::AfterSkirmishSetup(std::uint8_t* frontEnd) {
		if (installed && listWanted) OnSkirmishScreen(frontEnd);
	}

	const char* CustomCampaign::TextById(std::int32_t id) {
		if (id != CampaignHeadingText && id != WonHeadingText) return nullptr;
		// Made once in the game's text form; the caller keeps the pointer while it draws. The first heading follows the
		// list: "Campaign" or "Level".
		static char campaignText[0x40]{};
		static char levelText[0x40]{};
		static char wonText[0x40]{};
		const bool levels = id == CampaignHeadingText && LevelsListed();
		auto& text = id == WonHeadingText ? wonText : levels ? levelText : campaignText;
		if (text[0] == 0) {
			reinterpret_cast<ConvertTitleFn>(Game::Resolve(Game::Address::ConvertText))(id == WonHeadingText ? L"Won" : levels ? L"Level" : L"Campaign", text,
				static_cast<std::int32_t>(sizeof(text)));
		}
		return text;
	}

	bool CustomCampaign::OnGameComponentStart(bool playing) {
		if (!playing) {
			// Back from a level of a campaign: its level list, with the next level chosen after a win (the same one after a
			// loss). Nothing starts by itself.
			if (inSession && activeCampaign < campaigns.size()) {
				const auto& campaign = campaigns[activeCampaign];
				if (levelWon && campaign.WonCount() == campaign.levels.size()) Logger::Info("Custom campaign \"" + Narrow(campaign.name) + "\": every level won.");
				chosenCampaign = activeCampaign;
				wantedKind = ListKind::Levels;
				wantedRow = static_cast<std::int32_t>(levelWon && activeLevel + 1 < campaign.levels.size() ? activeLevel + 1 : activeLevel);
				listMode = true;
				listWanted = true;
			}
			inSession = false;
			armedForGame = false;
			redirectOn = false;
			briefingShown = false;
			return false;
		}
		const auto mode = Memory::Read<std::int32_t>(Game::Resolve(Game::Address::PlayMode));
		const bool fromSave = Memory::Read<std::int32_t>(Game::Resolve(Game::Address::GameConfigHasSaveFile)) != 0;
		if (!armedForGame || mode != Game::PlayModeSkirmish || fromSave) {
			armedForGame = false;
			inSession = false;
			redirectOn = false;
			resultsForOurs = false;
			return false;
		}
		armedForGame = false;
		inSession = true;
		resultsForOurs = true;
		resultSeen = false;
		levelWon = false;
		// The skirmish screen's lists of creatures, rooms and the rest would overrule what the level makes available.
		for (const auto count : { Game::Address::GameConfigCreatureCount, Game::Address::GameConfigRoomCount, Game::Address::GameConfigDoorCount,
			Game::Address::GameConfigTrapCount, Game::Address::GameConfigSpellCount }) {
			*reinterpret_cast<std::uint32_t*>(Game::Resolve(count)) = 0;
		}
		return true;
	}

	fs::path CustomCampaign::LevelFileFor(const fs::path& path) {
		if (!redirectOn || activeCampaign >= campaigns.size()) return {};
		const auto lower = StringUtils::ToLower(path.string());
		if (lower.find("maps\\") == std::string::npos && lower.find("maps/") == std::string::npos) return {};
		const auto extension = StringUtils::ToLower(path.extension().string());
		if (extension != ".kwd" && extension != ".kld") return {};
		std::error_code error;
		const auto candidate = campaigns[activeCampaign].folder / path.filename();
		return fs::is_regular_file(candidate, error) ? candidate : fs::path{};
	}

	void CustomCampaign::Install() {
		// sub esp, 0x11C; mov byte [0x73EDA0], 0
		const bool menu = Hook(Game::Resolve(Game::Address::FrontEndMenuAction), { 0x81, 0xEC, 0x1C, 0x01, 0x00, 0x00, 0xC6, 0x05, 0xA0, 0xED, 0x73, 0x00, 0x00 },
			reinterpret_cast<void*>(&MenuActionDetour), reinterpret_cast<void**>(&originalMenuAction));
		// mov eax, [esp+4]; sub esp, 0x20C; mov [0x740348], eax
		const bool click = menu && Hook(Game::Resolve(Game::Address::MapListClick), { 0x8B, 0x44, 0x24, 0x04, 0x81, 0xEC, 0x0C, 0x02, 0x00, 0x00, 0xA3, 0x48, 0x03, 0x74, 0x00 },
			reinterpret_cast<void*>(&ListClickDetour), reinterpret_cast<void**>(&originalListClick));
		const bool status = click && Hook(Game::Resolve(Game::Address::PlayerSetStatus), { 0x64, 0xA1, 0x00, 0x00, 0x00, 0x00, 0x6A, 0xFF, 0x68, 0xA0, 0xC5, 0x64, 0x00, 0x50 },
			reinterpret_cast<void*>(&SetStatusDetour), reinterpret_cast<void**>(&originalSetStatus));
		// mov eax, [0x7415B8]; test eax, eax; jz +0x14
		const bool texts = status && Hook(Game::Resolve(Game::Address::BriefingText), { 0xA1, 0xB8, 0x15, 0x74, 0x00, 0x85, 0xC0, 0x74, 0x14 },
			reinterpret_cast<void*>(&BriefingTextDetour), reinterpret_cast<void**>(&originalBriefingText));
		if (status && !texts) Logger::Warning("Custom campaign: unexpected game code at the briefing texts, campaign.yml's texts are not shown.");
		// push esi; mov esi, [esp+8]; push edi; push esi; push 9; push 1
		const bool step = texts && (SkirmishSwapKeeper::SetupHooked() || Hook(Game::Resolve(Game::Address::SkirmishScreenStep), { 0x56, 0x8B, 0x74, 0x24, 0x08, 0x57, 0x56, 0x6A, 0x09, 0x6A, 0x01 },
			reinterpret_cast<void*>(&SkirmishStepDetour), reinterpret_cast<void**>(&originalSkirmishStep)));
		// fs:[0] SEH prologue: mov eax, fs:[0]; push -1; push 0x64DEAB; push eax
		const bool details = texts && Hook(Game::Resolve(Game::Address::MapListDetails), { 0x64, 0xA1, 0x00, 0x00, 0x00, 0x00, 0x6A, 0xFF, 0x68, 0xAB, 0xDE, 0x64, 0x00, 0x50 },
			reinterpret_cast<void*>(&DetailsDetour), reinterpret_cast<void**>(&originalDetails));
		// sub esp, 0x26C; lea eax, [esp+4]; push ebx; push ebp; mov ebp, ecx
		const bool results = Hook(Game::Resolve(Game::Address::SkirmishResultsTable), { 0x81, 0xEC, 0x6C, 0x02, 0x00, 0x00, 0x8D, 0x44, 0x24, 0x04, 0x53, 0x55, 0x8B, 0xE9 },
			reinterpret_cast<void*>(&ResultsDetour), reinterpret_cast<void**>(&originalResults));
		if (!results) Logger::Warning("Custom campaign: unexpected game code at the results table; the results screen after a campaign level may crash.");
		if (texts && !AddBriefingImageWidget()) Logger::Warning("Custom campaign: unexpected briefing window; campaign.yml's briefing pictures are not shown.");
		if (texts && !details) Logger::Warning("Custom campaign: unexpected game code at the map details; the list shows \"Max Players\" for campaigns.");
		if (texts && !step) Logger::Warning("Custom campaign: unexpected game code at the skirmish screen's step; the skirmish screen may show before the campaign list.");
		if (!status || !FrameRate::IsPresentHooked()) {
			Logger::Warning("Custom campaign: unexpected game code (or no frame hook), custom campaigns are off.");
			return;
		}
		FrameRate::AddAfterPresent(&AfterPresent);
		installed = true;
		LoadCampaigns();
	}
}

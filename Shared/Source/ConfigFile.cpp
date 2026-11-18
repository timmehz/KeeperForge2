#include "ConfigFile.h"
#include <fstream>
#include <optional>
#include <ranges>
#include <sstream>
#include <string_view>
#include <system_error>
#include <utility>
namespace fs = std::filesystem;
namespace KeeperForge2 {
	namespace {
		// The whole file, or nothing if it could not be opened.
		std::optional<std::string> ReadText(const fs::path& path) {
			std::ifstream file(path, std::ios::binary);
			if (!file.is_open()) { return std::nullopt; }
			std::ostringstream content;
			content << file.rdbuf();
			return std::move(content).str();
		}
		std::string Trim(std::string_view text) {
			const auto first = text.find_first_not_of(" \t\r\n");
			if (first == std::string_view::npos) { return {}; }
			const auto last = text.find_last_not_of(" \t\r\n");
			return std::string(text.substr(first, last - first + 1));
		}
		// A '#' starts a comment at the beginning of a line or after whitespace.
		std::string_view StripComment(std::string_view line) {
			for (std::size_t i = 0; i < line.size(); ++i) {
				if (line[i] == '#' && (i == 0 || line[i - 1] == ' ' || line[i - 1] == '\t')) { return line.substr(0, i); }
			}
			return line;
		}
		std::string Unquote(const std::string& value) {
			if (value.size() >= 2 && (value.front() == '"' || value.front() == '\'') && value.back() == value.front()) {
				return value.substr(1, value.size() - 2);
			}
			return value;
		}
		std::vector<std::string> SplitLines(const std::string& text) {
			std::vector<std::string> lines;
			std::istringstream stream(text);
			std::string line;
			while (std::getline(stream, line)) {
				if (!line.empty() && line.back() == '\r') { line.pop_back(); }
				lines.push_back(line);
			}
			return lines;
		}
		// "# text" -> "text" (a header line's content).
		std::string HeaderLineText(const std::string& line) {
			std::string_view text = line;
			if (!text.empty() && text.front() == '#') { text.remove_prefix(1); }
			return Trim(text);
		}

		ConfigHeader ParseHeader(const std::vector<std::string>& lines) {
			ConfigHeader result;
			result.present = true;
			bool inDescription = false;
			for (const auto& line : lines) {
				const std::string text = HeaderLineText(line);
				if (text.starts_with("author:")) {
					inDescription = false;
					result.authors.append_range(std::string_view(text).substr(7) | std::views::split(',')
						| std::views::transform([](auto author) { return Trim(std::string_view(author)); })
						| std::views::filter([](const std::string& author) { return !author.empty(); }));
				}
				else if (text.starts_with("description:")) {
					inDescription = true;
					result.description = Trim(std::string_view(text).substr(12));
				}
				else if (inDescription && !text.empty()) {
					result.description += (result.description.empty() ? "" : "\n") + text;
				}
			}
			return result;
		}
		void WriteComment(std::string& out, const std::string& comment) {
			if (comment.empty()) { return; }
			std::istringstream stream(comment);
			std::string line;
			while (std::getline(stream, line)) { out += line.empty() ? "#\n" : "# " + line + "\n"; }
		}
	}

	bool ConfigFile::Load(const fs::path& path, const std::vector<ConfigSetting>& settings, const std::string& defaultHeader) {
		this->path = path;
		values.clear();
		header = {};
		removed.clear();
		reset.clear();
		std::error_code error;
		std::string original;
		const bool exists = fs::exists(path, error);
		if (exists) {
			auto text = ReadText(path);
			if (!text) { return false; }
			original = std::move(*text);
		}
		const auto lines = SplitLines(original);

		// The header block, if the file starts with one (blank lines before it are allowed).
		std::size_t body = 0;
		while (body < lines.size() && Trim(lines[body]).empty()) { ++body; }
		std::vector<std::string> headerLines;
		if (body < lines.size() && Trim(lines[body]) == HeaderStart) {
			std::size_t end = body + 1;
			while (end < lines.size() && Trim(lines[end]) != HeaderEnd) { ++end; }
			if (end < lines.size()) {
				headerLines.assign(lines.begin() + static_cast<std::ptrdiff_t>(body), lines.begin() + static_cast<std::ptrdiff_t>(end) + 1);
				header = ParseHeader(headerLines);
				body = end + 1;
			}
			else { body = 0; }
		}
		else { body = 0; }

		// The values, and the text as written in the file (quotes kept) to write them back unchanged.
		std::unordered_map<std::string, std::string> written;
		std::vector<std::string> fileOrder;
		for (std::size_t i = body; i < lines.size(); ++i) {
			const std::string content = Trim(StripComment(lines[i]));
			const auto colon = content.find(':');
			if (content.empty() || colon == std::string::npos) { continue; }
			const std::string key = Trim(std::string_view(content).substr(0, colon));
			if (key.empty()) { continue; }
			const std::string value = Trim(std::string_view(content).substr(colon + 1));
			if (!written.contains(key)) { fileOrder.push_back(key); }
			written[key] = value;
			values[key] = Unquote(value);
		}

		std::string updated;
		if (!headerLines.empty()) {
			for (const auto& line : headerLines) { updated += line + "\n"; }
		}
		else {
			updated += defaultHeader;
			if (!defaultHeader.empty() && defaultHeader.back() != '\n') { updated += "\n"; }
		}
		if (settings.empty()) {
			// Nothing to check the values against: keep everything after the header as it is.
			for (std::size_t i = headerLines.empty() ? 0 : body; i < lines.size(); ++i) { updated += lines[i] + "\n"; }
			if (!headerLines.empty() || defaultHeader.empty()) {
				// Only the header could change, and it did not.
				if (exists) { return true; }
			}
		}
		else {
			std::unordered_map<std::string, std::string> kept;
			for (const auto& setting : settings) {
				// A blank line only before a setting with its own comment: the ones after it without one (such as the
				// other panel keys) stay right under it as a group.
				if (!setting.comment.empty()) { updated += "\n"; }
				WriteComment(updated, setting.comment);
				auto found = written.find(setting.key);
				if (found != written.end() && setting.valid && !setting.valid(values[setting.key])) {
					reset.push_back({ setting.key, values[setting.key] });
					found = written.end();
				}
				updated += setting.key + ": " + (found != written.end() ? found->second : setting.defaultValue) + "\n";
				if (found == written.end()) { values[setting.key] = setting.defaultValue; }
				kept[setting.key] = values[setting.key];
			}
			for (const auto& key : fileOrder) {
				if (!kept.contains(key)) { removed.push_back({ key, values[key] }); }
			}
			values = std::move(kept);
		}

		if (exists && updated == original) { return true; }
		std::ofstream output(path, std::ios::binary | std::ios::trunc);
		if (!output.is_open()) { return false; }
		output << updated;
		return static_cast<bool>(output);
	}

	std::string ConfigFile::Get(const std::string& key, const std::string& defaultValue) const {
		const auto it = values.find(key);
		return it != values.end() ? it->second : defaultValue;
	}
	bool ConfigFile::Has(const std::string& key) const { return values.contains(key); }

	bool ConfigFile::Set(const std::string& key, const std::string& value) {
		if (path.empty()) { return false; }
		const auto lines = SplitLines(ReadText(path).value_or(std::string{}));
		std::string updated;
		bool found = false;
		for (const auto& line : lines) {
			const std::string content = Trim(StripComment(line));
			const auto colon = content.find(':');
			// Header lines start with '#', so they never match a key.
			if (!found && colon != std::string::npos && Trim(std::string_view(content).substr(0, colon)) == key) {
				// Keep what came before the key (indentation) and any comment after the value.
				const auto keyStart = line.find(key);
				const auto stripped = StripComment(line);
				const std::string comment = line.substr(stripped.size());
				updated += line.substr(0, keyStart) + key + ": " + value + (comment.empty() ? "" : " " + Trim(comment)) + "\n";
				found = true;
				continue;
			}
			updated += line + "\n";
		}
		if (!found) { updated += key + ": " + value + "\n"; }
		std::ofstream output(path, std::ios::binary | std::ios::trunc);
		if (!output.is_open()) { return false; }
		output << updated;
		if (!output) { return false; }
		values[key] = value;
		return true;
	}

	std::string ConfigFile::MakeHeader(const std::string& authors, const std::string& description) {
		std::string text = std::string(HeaderStart) + "\n# author: " + authors + "\n";
		const auto lines = SplitLines(description);
		if (lines.empty()) { text += "# description:\n"; }
		for (std::size_t i = 0; i < lines.size(); ++i) { text += (i == 0 ? "# description: " : "# ") + lines[i] + "\n"; }
		return text + HeaderEnd + "\n";
	}
}

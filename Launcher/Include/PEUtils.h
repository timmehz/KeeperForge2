#pragma once
#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>
#include <string_view>
namespace KeeperForge2{
    class PEUtils {
        public:
            enum class LargeAddressResult { CopyReady, SourceAlreadyAware };
            // The errors below say what went wrong, for the log.
            static std::expected<std::uint32_t, std::string_view> GetExportRva(const std::filesystem::path& filePath,const std::string& exportName);
            // Makes copy the same as the 32-bit exe source with only the large address aware flag set, writing it only if it
            // differs. If source has the flag already, writes nothing.
            static std::expected<LargeAddressResult, std::string_view> MakeLargeAddressAwareCopy(const std::filesystem::path& source,const std::filesystem::path& copy);
    };
}

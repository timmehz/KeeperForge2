#include "PEUtils.h"
#include <Windows.h>
#include <fstream>
#include <vector>

namespace KeeperForge2 {
    namespace {
        constexpr std::string_view NotReadable = "the file could not be read";
        constexpr std::string_view NotExe = "not an exe or DLL";
        constexpr std::string_view Not32Bit = "not a 32-bit file";
        constexpr std::string_view NoExports = "the file exports nothing";
        constexpr std::string_view BadExports = "its export table is damaged";
        bool ReadWholeFile(const std::filesystem::path& filePath, std::vector<std::uint8_t>& data) {
            std::ifstream file(filePath, std::ios::binary | std::ios::ate);
            if (!file) { return false; }
            const std::streamoff size = file.tellg();
            if (size <= 0) { return false; }
            file.seekg(0);
            data.resize(static_cast<std::size_t>(size));
            return static_cast<bool>(file.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size())));
        }
    }
    std::expected<PEUtils::LargeAddressResult, std::string_view> PEUtils::MakeLargeAddressAwareCopy(const std::filesystem::path& source, const std::filesystem::path& copy) {
        std::vector<std::uint8_t> data;
        if (!ReadWholeFile(source, data)) { return std::unexpected(NotReadable); }
        if (data.size() < sizeof(IMAGE_DOS_HEADER)) { return std::unexpected(NotExe); }
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(data.data());
        if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0) { return std::unexpected(NotExe); }
        const auto ntOffset = static_cast<std::size_t>(dos->e_lfanew);
        if (ntOffset + sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER) > data.size()) { return std::unexpected(NotExe); }
        if (*reinterpret_cast<const DWORD*>(data.data() + ntOffset) != IMAGE_NT_SIGNATURE) { return std::unexpected(NotExe); }
        auto* fileHeader = reinterpret_cast<IMAGE_FILE_HEADER*>(data.data() + ntOffset + sizeof(DWORD));
        if (fileHeader->Machine != IMAGE_FILE_MACHINE_I386) { return std::unexpected(Not32Bit); }
        if (fileHeader->Characteristics & IMAGE_FILE_LARGE_ADDRESS_AWARE) { return LargeAddressResult::SourceAlreadyAware; }
        // The header's checksum is left as it is: Windows does not check it for programs.
        fileHeader->Characteristics |= IMAGE_FILE_LARGE_ADDRESS_AWARE;
        std::vector<std::uint8_t> existing;
        if (ReadWholeFile(copy, existing) && existing == data) { return LargeAddressResult::CopyReady; }
        std::ofstream output(copy, std::ios::binary | std::ios::trunc);
        output.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
        output.close();
        if (!output.good()) { return std::unexpected(std::string_view("the copy could not be written")); }
        return LargeAddressResult::CopyReady;
    }
    std::expected<std::uint32_t, std::string_view> PEUtils::GetExportRva(const std::filesystem::path& filePath, const std::string& exportName) {
        std::vector<std::uint8_t> data;
        if (!ReadWholeFile(filePath, data)) { return std::unexpected(NotReadable); }
        if (data.size() < sizeof(IMAGE_DOS_HEADER)) { return std::unexpected(NotExe); }
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(data.data());
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) { return std::unexpected(NotExe); }
        if (dos->e_lfanew < 0) { return std::unexpected(NotExe); }
        const auto ntOffset = static_cast<std::size_t>(dos->e_lfanew);
        if (ntOffset + sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER) > data.size()) { return std::unexpected(NotExe); }
        const auto* fileHeader = reinterpret_cast<const IMAGE_FILE_HEADER*>(data.data() + ntOffset + sizeof(DWORD));
        if (fileHeader->Machine != IMAGE_FILE_MACHINE_I386) { return std::unexpected(Not32Bit); }
        const auto* optionalHeader = reinterpret_cast<const IMAGE_OPTIONAL_HEADER32*>(data.data() + ntOffset + sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER));
        if (optionalHeader->Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC) { return std::unexpected(Not32Bit); }
        const auto& directory = optionalHeader->DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
        if (directory.VirtualAddress == 0) { return std::unexpected(NoExports); }
        const auto* section = IMAGE_FIRST_SECTION(reinterpret_cast<const IMAGE_NT_HEADERS32*>(data.data() + ntOffset));
        const WORD sectionCount = fileHeader->NumberOfSections;
        auto RvaToOffset = [&](std::uint32_t targetRva, std::size_t& offset) -> bool {
            for (WORD i = 0; i < sectionCount; ++i) {
                const auto& current = section[i];
                const std::uint32_t start = current.VirtualAddress;
                const std::uint32_t size = current.SizeOfRawData;
                if (targetRva < start || targetRva >= start + size) { continue; }
                    const std::uint32_t delta = targetRva - start;
                    offset = static_cast<std::size_t>(current.PointerToRawData + delta);
                    return offset < data.size();
                }
                return false;
            };
        std::size_t exportOffset = 0;
        if (!RvaToOffset(directory.VirtualAddress, exportOffset)) { return std::unexpected(BadExports); }
        if (exportOffset + sizeof(IMAGE_EXPORT_DIRECTORY) > data.size()) { return std::unexpected(BadExports); }
        const auto* exportDirectory = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(data.data() + exportOffset);
        if (exportDirectory->NumberOfNames == 0) { return std::unexpected(NoExports); }
        std::size_t namesOffset = 0, ordinalsOffset = 0, functionsOffset = 0;
        if (!RvaToOffset(exportDirectory->AddressOfNames, namesOffset)) { return std::unexpected(BadExports); }
        if (!RvaToOffset(exportDirectory->AddressOfNameOrdinals, ordinalsOffset)) { return std::unexpected(BadExports); }
        if (!RvaToOffset(exportDirectory->AddressOfFunctions, functionsOffset)) { return std::unexpected(BadExports); }
        const auto* names = reinterpret_cast<const std::uint32_t*>(data.data() + namesOffset);
        const auto* ordinals = reinterpret_cast<const std::uint16_t*>(data.data() + ordinalsOffset);
        const auto* functions = reinterpret_cast<const std::uint32_t*>(data.data() + functionsOffset);
        for (std::uint32_t i = 0; i < exportDirectory->NumberOfNames; ++i) {
            std::size_t nameOffset = 0;
            if (!RvaToOffset(names[i], nameOffset)) { return std::unexpected(BadExports); }
            const char* name = reinterpret_cast<const char*>(data.data() + nameOffset);
            if (exportName == name) {
                const std::uint16_t ordinal = ordinals[i];
                if (ordinal >= exportDirectory->NumberOfFunctions) { return std::unexpected(BadExports); }
                const std::uint32_t rva = functions[ordinal];
                if (rva == 0) { return std::unexpected(BadExports); }
                return rva;
            }
        }
        return std::unexpected(std::string_view("no export of that name"));
    }
}

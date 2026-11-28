// Standalone 0.3.DL cache archive extractor (.7z & .rar, RAR4/RAR5, solid archives).
//
// Third-party attribution notes:
// - 7-Zip C SDK (Deps/7zip): Igor Pavlov, Public Domain.
// - UnRAR (Deps/Unrar): Copyright (c) Alexander L. Roshal.
//   UnRAR source code may be used in any software to handle RAR archives without
//   limitations free of charge, but cannot be used to develop RAR (WinRAR) compatible
//   archiver and to re-create RAR compression algorithm, which is proprietary.
//   Distribution of modified UnRAR source code in separate form or as a part of other
//   software is permitted, provided that full text of this paragraph, starting from
//   "UnRAR source code" words, is included in license, or in documentation if license
//   is not available, and in source code comments of resulting package.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace samparchive {

enum class ArchiveFormat : uint8_t {
	Unknown = 0,
	SevenZip = 1,
	Rar = 2,
};

struct CacheArchiveResult {
	bool ok = false;
	bool cancelled = false;
	bool passwordRequired = false;
	int importedFiles = 0;
	int skippedFiles = 0;
	uint64_t importedBytes = 0;
	std::vector<std::string> nestedZipPaths;
	std::string error;
};

// Returns false from callback when the caller has requested cancellation.
using CacheProgressCallback = std::function<bool(int importedFiles, int skippedFiles, const std::string& relPath)>;

ArchiveFormat detectArchiveFormat(const std::string& archivePath);

// Resolves an archive entry path (arbitrary wrapper folders, Windows/Unix separators,
// Chinese directories, <ip:port> or <ip.port> folders) to a canonical 0.3.DL cache filename
// ("0x<HEX>.dff" or "0x<HEX>.txd"). Returns empty string if unsafe (Zip-Slip "..") or not
// a 0x<CRC>.dff|txd server cache file.
std::string resolveCanonicalCacheLeaf(const std::string& rawEntryPath);

// Extracts all valid 0x<CRC>.dff|txd entries from a .7z or .rar (RAR4/RAR5, solid or non-solid)
// archive into localCacheDir ("0x%X.dff" / "0x%X.txd"). Supports nested .7z/.rar archives up to
// depth 2, and stages any nested .zip files into localCacheDir (returned via nestedZipPaths).
CacheArchiveResult extractCacheArchive(
	const std::string& archivePath,
	const std::string& localCacheDir,
	const CacheProgressCallback& onProgress = nullptr);

} // namespace samparchive

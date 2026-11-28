// Standalone 0.3.DL cache archive extractor (.7z & .rar, RAR4/RAR5, solid archives).
//
// UnRAR source code may be used in any software to handle RAR archives without
// limitations free of charge, but cannot be used to develop RAR (WinRAR) compatible
// archiver and to re-create RAR compression algorithm, which is proprietary.
// Distribution of modified UnRAR source code in separate form or as a part of other
// software is permitted, provided that full text of this paragraph, starting from
// "UnRAR source code" words, is included in license, or in documentation if license
// is not available, and in source code comments of resulting package.

#include "ArchiveExtractor.h"

// 7-Zip C SDK (Public Domain)
#include "7z.h"
#include "7zAlloc.h"
#include "7zCrc.h"
#include "7zFile.h"

// RARLAB UnRAR library interface (RARDLL)
#if !defined(_WIN32) && !defined(_UNIX)
#define _UNIX
#endif
#include "dll.hpp"

#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#if defined(_WIN32)
#include <direct.h>
#include <windows.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace samparchive {

namespace {

constexpr uint64_t kMaxCacheFileBytes = 64ull * 1024ull * 1024ull;      // 64 MiB per .dff/.txd
constexpr uint64_t kMaxNestedArchiveBytes = 512ull * 1024ull * 1024ull; // 512 MiB nested archive cap
constexpr int kMaxArchiveNesting = 2;

#if UINTPTR_MAX > 0xFFFFFFFFu
constexpr size_t kMax7zSolidBlockBytes = 1536ull * 1024ull * 1024ull; // 1.5 GiB on 64-bit
#else
constexpr size_t kMax7zSolidBlockBytes = 384u * 1024u * 1024u;        // 384 MiB on 32-bit
#endif

std::once_flag g_crcInitOnce;
std::mutex g_unrarMutex; // UnRAR uses global ErrHandler state; serialize RARDLL sessions safely.

void* boundedSzAlloc(ISzAllocPtr, size_t size)
{
	if (size == 0) {
		return nullptr;
	}
	if (size > kMax7zSolidBlockBytes) {
		return nullptr;
	}
	return std::malloc(size);
}

void boundedSzFree(ISzAllocPtr, void* address)
{
	std::free(address);
}

const ISzAlloc g_boundedAlloc = {boundedSzAlloc, boundedSzFree};

void ensureDirExists(const std::string& dir)
{
	if (dir.empty()) return;
	std::string cur;
	cur.reserve(dir.size());
	for (size_t i = 0; i < dir.size(); ++i) {
		const char ch = dir[i];
		cur.push_back(ch);
		if ((ch == '/' || ch == '\\') && cur.size() > 1) {
#if defined(_WIN32)
			_mkdir(cur.c_str());
#else
			mkdir(cur.c_str(), 0755);
#endif
		}
	}
#if defined(_WIN32)
	_mkdir(dir.c_str());
#else
	mkdir(dir.c_str(), 0755);
#endif
}

void removeFileQuiet(const std::string& path)
{
	if (path.empty()) return;
#if defined(_WIN32)
	std::remove(path.c_str());
#else
	unlink(path.c_str());
#endif
}

bool renameOverwrite(const std::string& src, const std::string& dst)
{
	removeFileQuiet(dst);
	return std::rename(src.c_str(), dst.c_str()) == 0;
}

void appendUtf8CodePoint(std::string& out, uint32_t cp)
{
	if (cp <= 0x7Fu) {
		out.push_back(static_cast<char>(cp));
	} else if (cp <= 0x7FFu) {
		out.push_back(static_cast<char>(0xC0u | (cp >> 6)));
		out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
	} else if (cp <= 0xFFFFu) {
		out.push_back(static_cast<char>(0xE0u | (cp >> 12)));
		out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
		out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
	} else if (cp <= 0x10FFFFu) {
		out.push_back(static_cast<char>(0xF0u | (cp >> 18)));
		out.push_back(static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu)));
		out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
		out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
	}
}

std::string utf16ToUtf8(const UInt16* u16, size_t count)
{
	std::string out;
	if (!u16 || count == 0) return out;
	out.reserve(count * 3);
	for (size_t i = 0; i < count; ++i) {
		const uint32_t w1 = u16[i];
		if (w1 == 0) break;
		if (w1 >= 0xD800u && w1 <= 0xDBFFu && (i + 1) < count) {
			const uint32_t w2 = u16[i + 1];
			if (w2 >= 0xDC00u && w2 <= 0xDFFFu) {
				const uint32_t cp = 0x10000u + (((w1 - 0xD800u) << 10) | (w2 - 0xDC00u));
				appendUtf8CodePoint(out, cp);
				++i;
				continue;
			}
		}
		appendUtf8CodePoint(out, w1);
	}
	return out;
}

std::string wideToUtf8(const wchar_t* ws)
{
	std::string out;
	if (!ws) return out;
	for (size_t i = 0; ws[i] != 0; ++i) {
		const uint32_t w1 = static_cast<uint32_t>(ws[i]);
#if WCHAR_MAX <= 0xFFFFu
		if (w1 >= 0xD800u && w1 <= 0xDBFFu && ws[i + 1] != 0) {
			const uint32_t w2 = static_cast<uint32_t>(ws[i + 1]);
			if (w2 >= 0xDC00u && w2 <= 0xDFFFu) {
				appendUtf8CodePoint(out, 0x10000u + (((w1 - 0xD800u) << 10) | (w2 - 0xDC00u)));
				++i;
				continue;
			}
		}
#endif
		appendUtf8CodePoint(out, w1);
	}
	return out;
}

std::wstring utf8ToWide(const std::string& s)
{
	std::wstring out;
	out.reserve(s.size());
	size_t i = 0;
	while (i < s.size()) {
		const uint8_t b0 = static_cast<uint8_t>(s[i]);
		uint32_t cp = 0;
		if ((b0 & 0x80u) == 0) {
			cp = b0;
			i += 1;
		} else if ((b0 & 0xE0u) == 0xC0u && (i + 1) < s.size()) {
			cp = ((b0 & 0x1Fu) << 6) | (static_cast<uint8_t>(s[i + 1]) & 0x3Fu);
			i += 2;
		} else if ((b0 & 0xF0u) == 0xE0u && (i + 2) < s.size()) {
			cp = ((b0 & 0x0Fu) << 12)
				| ((static_cast<uint8_t>(s[i + 1]) & 0x3Fu) << 6)
				| (static_cast<uint8_t>(s[i + 2]) & 0x3Fu);
			i += 3;
		} else if ((b0 & 0xF8u) == 0xF0u && (i + 3) < s.size()) {
			cp = ((b0 & 0x07u) << 18)
				| ((static_cast<uint8_t>(s[i + 1]) & 0x3Fu) << 12)
				| ((static_cast<uint8_t>(s[i + 2]) & 0x3Fu) << 6)
				| (static_cast<uint8_t>(s[i + 3]) & 0x3Fu);
			i += 4;
		} else {
			cp = b0;
			i += 1;
		}
#if WCHAR_MAX <= 0xFFFFu
		if (cp > 0xFFFFu) {
			cp -= 0x10000u;
			out.push_back(static_cast<wchar_t>(0xD800u + (cp >> 10)));
			out.push_back(static_cast<wchar_t>(0xDC00u + (cp & 0x3FFu)));
		} else {
			out.push_back(static_cast<wchar_t>(cp));
		}
#else
		out.push_back(static_cast<wchar_t>(cp));
#endif
	}
	return out;
}

std::string stripDuplicateStem(const std::string& rawStem)
{
	size_t start = 0;
	while (start < rawStem.size() && rawStem[start] == ' ') {
		++start;
	}
	size_t end = rawStem.size();
	while (end > start && rawStem[end - 1] == ' ') {
		--end;
	}
	const std::string trimmed = rawStem.substr(start, end - start);
	if (trimmed.size() > 5 && trimmed.back() == ')') {
		const size_t open = trimmed.rfind('(');
		if (open != std::string::npos && open > 2 && open + 1 < trimmed.size() - 1) {
			bool allDigits = true;
			for (size_t i = open + 1; i + 1 < trimmed.size(); ++i) {
				if (trimmed[i] < '0' || trimmed[i] > '9') {
					allDigits = false;
					break;
				}
			}
			if (allDigits) {
				size_t p = open;
				while (p > 0 && trimmed[p - 1] == ' ') {
					--p;
				}
				return trimmed.substr(0, p);
			}
		}
	}
	return trimmed;
}

std::string extractSafeEntryLeaf(const std::string& rawEntryPath)
{
	if (rawEntryPath.empty()) return {};
	std::string unified = rawEntryPath;
	for (char& ch : unified) {
		if (ch == '\\') ch = '/';
	}
	size_t pos = 0;
	while (pos < unified.size() && unified[pos] == '/') {
		++pos;
	}
	if (pos >= unified.size() || unified.back() == '/') {
		return {};
	}
	std::string leaf;
	while (pos < unified.size()) {
		size_t slash = unified.find('/', pos);
		const std::string seg = (slash == std::string::npos)
			? unified.substr(pos)
			: unified.substr(pos, slash - pos);
		if (seg == "..") {
			return {}; // Zip-Slip guard
		}
		if (!seg.empty() && seg != ".") {
			leaf = seg;
		}
		if (slash == std::string::npos) break;
		pos = slash + 1;
	}
	return leaf;
}

enum class NestedKind : uint8_t {
	None = 0,
	Zip = 1,
	SevenZip = 2,
	Rar = 3,
};

NestedKind classifyNestedArchiveLeaf(const std::string& leaf)
{
	const size_t dot = leaf.rfind('.');
	if (dot == std::string::npos || dot == 0 || dot + 1 >= leaf.size()) {
		return NestedKind::None;
	}
	std::string ext = leaf.substr(dot + 1);
	for (char& ch : ext) {
		ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
	}
	if (ext == "zip") return NestedKind::Zip;
	if (ext == "7z") return NestedKind::SevenZip;
	if (ext == "rar") return NestedKind::Rar;
	return NestedKind::None;
}

bool writeBufferAtomically(const uint8_t* data, size_t len, const std::string& dstPath)
{
	if (!data || len == 0) return false;
	const std::string tmpPath = dstPath + ".imp.tmp";
	FILE* fp = std::fopen(tmpPath.c_str(), "wb");
	if (!fp) return false;
	const size_t written = std::fwrite(data, 1, len, fp);
	const bool flushed = (std::fflush(fp) == 0) && !std::ferror(fp);
	std::fclose(fp);
	if (written != len || !flushed) {
		removeFileQuiet(tmpPath);
		return false;
	}
	if (!renameOverwrite(tmpPath, dstPath)) {
		removeFileQuiet(tmpPath);
		return false;
	}
	return true;
}

void extractArchiveInternal(
	const std::string& archivePath,
	const std::string& localCacheDir,
	int depth,
	int& nestedSeq,
	CacheArchiveResult& acc,
	const CacheProgressCallback& onProgress);

void extract7zInternal(
	const std::string& archivePath,
	const std::string& localCacheDir,
	int depth,
	int& nestedSeq,
	CacheArchiveResult& acc,
	const CacheProgressCallback& onProgress)
{
	std::call_once(g_crcInitOnce, []() { CrcGenerateTable(); });

	CFileInStream archiveStream{};
	CLookToRead2 lookStream{};
	CSzArEx db{};
	FileInStream_CreateVTable(&archiveStream);
	File_Construct(&archiveStream.file);
	LookToRead2_CreateVTable(&lookStream, False);

	constexpr size_t kLookBufSize = 1u << 18;
	Byte* lookBuf = static_cast<Byte*>(std::malloc(kLookBufSize));
	if (!lookBuf) {
		acc.ok = false;
		acc.error = "内存分配失败";
		return;
	}
	lookStream.buf = lookBuf;
	lookStream.bufSize = kLookBufSize;
	lookStream.realStream = &archiveStream.vt;
	LookToRead2_INIT(&lookStream);

#ifdef USE_WINDOWS_FILE
	const std::wstring wPath = utf8ToWide(archivePath);
	const WRes openWRes = InFile_OpenW(&archiveStream.file, wPath.c_str());
#else
	const WRes openWRes = InFile_Open(&archiveStream.file, archivePath.c_str());
#endif
	if (openWRes != 0) {
		std::free(lookBuf);
		acc.ok = false;
		acc.error = "无法打开 .7z 压缩包";
		return;
	}

	SzArEx_Init(&db);
	SRes res = SzArEx_Open(&db, &lookStream.vt, &g_boundedAlloc, &g_boundedAlloc);
	if (res != SZ_OK) {
		if (res == SZ_ERROR_UNSUPPORTED) {
			acc.passwordRequired = true;
			acc.error = ".7z 压缩包使用了加密或暂不支持的过滤算法";
		} else {
			acc.error = ".7z 压缩包损坏或格式无效";
		}
		SzArEx_Free(&db, &g_boundedAlloc);
		File_Close(&archiveStream.file);
		std::free(lookBuf);
		acc.ok = false;
		return;
	}

	UInt32 blockIndex = 0xFFFFFFFFu;
	Byte* outBuffer = nullptr;
	size_t outBufferSize = 0;
	std::vector<UInt16> nameBuf;

	for (UInt32 i = 0; i < db.NumFiles; ++i) {
		if (onProgress && !onProgress(acc.importedFiles, acc.skippedFiles, "")) {
			acc.cancelled = true;
			break;
		}
		if (SzArEx_IsDir(&db, i)) {
			continue;
		}

		const size_t nameLen = SzArEx_GetFileNameUtf16(&db, i, nullptr);
		if (nameLen == 0 || nameLen > 4096u) {
			++acc.skippedFiles;
			continue;
		}
		nameBuf.resize(nameLen);
		SzArEx_GetFileNameUtf16(&db, i, nameBuf.data());
		const std::string entryPath = utf16ToUtf8(nameBuf.data(), nameLen);
		const UInt64 unpackSize = SzArEx_GetFileSize(&db, i);

		const std::string canonical = resolveCanonicalCacheLeaf(entryPath);
		if (!canonical.empty()) {
			if (unpackSize == 0 || unpackSize > kMaxCacheFileBytes) {
				++acc.skippedFiles;
				continue;
			}
			size_t offset = 0;
			size_t outSizeProcessed = 0;
			res = SzArEx_Extract(&db, &lookStream.vt, i, &blockIndex, &outBuffer, &outBufferSize,
				&offset, &outSizeProcessed, &g_boundedAlloc, &g_boundedAlloc);
			if (res != SZ_OK) {
				if (res == SZ_ERROR_UNSUPPORTED) {
					acc.passwordRequired = true;
					acc.error = ".7z 压缩包设有密码保护或使用了不支持的压缩方法";
				} else if (res == SZ_ERROR_MEM) {
					acc.error = ".7z 固实分块超出内存安全上限，请先用文件管理器解压后导入文件夹";
				} else {
					acc.error = ".7z 解压校验失败";
				}
				acc.ok = false;
				break;
			}
			const std::string dstPath = localCacheDir + "/" + canonical;
			if (outSizeProcessed > 0 && writeBufferAtomically(outBuffer + offset, outSizeProcessed, dstPath)) {
				++acc.importedFiles;
				acc.importedBytes += outSizeProcessed;
				if (onProgress && !onProgress(acc.importedFiles, acc.skippedFiles, "local/" + canonical)) {
					acc.cancelled = true;
					break;
				}
			} else {
				++acc.skippedFiles;
			}
			continue;
		}

		const std::string leaf = extractSafeEntryLeaf(entryPath);
		const NestedKind nested = (depth < kMaxArchiveNesting) ? classifyNestedArchiveLeaf(leaf) : NestedKind::None;
		if (nested != NestedKind::None && unpackSize > 0 && unpackSize <= kMaxNestedArchiveBytes) {
			size_t offset = 0;
			size_t outSizeProcessed = 0;
			res = SzArEx_Extract(&db, &lookStream.vt, i, &blockIndex, &outBuffer, &outBufferSize,
				&offset, &outSizeProcessed, &g_boundedAlloc, &g_boundedAlloc);
			if (res == SZ_OK && outSizeProcessed > 0) {
				++nestedSeq;
				char tag[64];
				std::snprintf(tag, sizeof(tag), "/.nested_%d_%d.%s.tmp", depth, nestedSeq,
					nested == NestedKind::Zip ? "zip" : "arc");
				const std::string nestedPath = localCacheDir + tag;
				if (writeBufferAtomically(outBuffer + offset, outSizeProcessed, nestedPath)) {
					if (nested == NestedKind::Zip) {
						acc.nestedZipPaths.push_back(nestedPath);
					} else {
						extractArchiveInternal(nestedPath, localCacheDir, depth + 1, nestedSeq, acc, onProgress);
						removeFileQuiet(nestedPath);
						if (!acc.ok || acc.cancelled) {
							break;
						}
					}
					continue;
				}
			}
		}
		++acc.skippedFiles;
	}

	ISzAlloc_Free(&g_boundedAlloc, outBuffer);
	SzArEx_Free(&db, &g_boundedAlloc);
	File_Close(&archiveStream.file);
	std::free(lookBuf);
}

struct RarStreamSink {
	FILE* fp = nullptr;
	uint64_t written = 0;
	uint64_t maxBytes = kMaxCacheFileBytes;
	bool writeError = false;
	bool passwordPrompted = false;
	bool cancelled = false;
	const CacheProgressCallback* onProgress = nullptr;
	int importedSoFar = 0;
	int skippedSoFar = 0;
};

int CALLBACK unrarCallback(UINT msg, LPARAM userData, LPARAM p1, LPARAM p2)
{
	RarStreamSink* sink = reinterpret_cast<RarStreamSink*>(userData);
	if (!sink) return -1;

	if (msg == UCM_NEEDPASSWORD || msg == UCM_NEEDPASSWORDW) {
		sink->passwordPrompted = true;
		return -1;
	}
	if (msg == UCM_CHANGEVOLUME || msg == UCM_CHANGEVOLUMEW) {
		// Multi-part missing volume: never block waiting for console prompt.
		return -1;
	}
	if (msg == UCM_LARGEDICT) {
		// Allow dictionaries up to 512 MiB; reject larger memory bombs.
		const uint64_t dictBytes = static_cast<uint64_t>(p1);
		return (dictBytes <= 512ull * 1024ull * 1024ull) ? 1 : -1;
	}
	if (msg == UCM_PROCESSDATA) {
		if (sink->onProgress && *sink->onProgress) {
			if (!(*sink->onProgress)(sink->importedSoFar, sink->skippedSoFar, "")) {
				sink->cancelled = true;
				return -1;
			}
		}
		if (!sink->fp || p2 <= 0) {
			return 1;
		}
		const size_t chunkLen = static_cast<size_t>(p2);
		if (sink->written + chunkLen > sink->maxBytes) {
			sink->writeError = true;
			return -1;
		}
		const uint8_t* ptr = reinterpret_cast<const uint8_t*>(p1);
		if (std::fwrite(ptr, 1, chunkLen, sink->fp) != chunkLen) {
			sink->writeError = true;
			return -1;
		}
		sink->written += chunkLen;
		return 1;
	}
	return 1;
}

struct PendingNestedArchive {
	NestedKind kind = NestedKind::None;
	std::string tempPath;
};

void extractRarInternal(
	const std::string& archivePath,
	const std::string& localCacheDir,
	int depth,
	int& nestedSeq,
	CacheArchiveResult& acc,
	const CacheProgressCallback& onProgress)
{
	std::vector<PendingNestedArchive> pendingNested;

	{
		std::lock_guard<std::mutex> lock(g_unrarMutex);

		RarStreamSink sink{};
		sink.onProgress = &onProgress;

		std::wstring wArcPath = utf8ToWide(archivePath);
		RAROpenArchiveDataEx openData{};
		openData.ArcName = const_cast<char*>(archivePath.c_str());
		openData.ArcNameW = wArcPath.empty() ? nullptr : wArcPath.data();
		openData.OpenMode = RAR_OM_EXTRACT;
		openData.Callback = unrarCallback;
		openData.UserData = reinterpret_cast<LPARAM>(&sink);
		openData.MaxAllocate = 512; // 512 MiB allocation ceiling

		HANDLE hArc = RAROpenArchiveEx(&openData);
		if (!hArc || openData.OpenResult != ERAR_SUCCESS) {
			if (openData.OpenResult == ERAR_MISSING_PASSWORD || openData.OpenResult == ERAR_BAD_PASSWORD
				|| sink.passwordPrompted) {
				acc.passwordRequired = true;
				acc.error = ".rar 压缩包设有密码保护";
			} else {
				acc.error = "无法读取 .rar 压缩包（文件损坏或分卷缺失）";
			}
			if (hArc) RARCloseArchive(hArc);
			acc.ok = false;
			return;
		}

		if ((openData.Flags & ROADF_ENCHEADERS) != 0) {
			RARCloseArchive(hArc);
			acc.passwordRequired = true;
			acc.ok = false;
			acc.error = ".rar 压缩包已加密文件名与内容";
			return;
		}

		RARHeaderDataEx hdr{};
		while (true) {
			sink.importedSoFar = acc.importedFiles;
			sink.skippedSoFar = acc.skippedFiles;
			const int hdrCode = RARReadHeaderEx(hArc, &hdr);
			if (hdrCode == ERAR_END_ARCHIVE) {
				break;
			}
			if (hdrCode != ERAR_SUCCESS) {
				if (hdrCode == ERAR_MISSING_PASSWORD || hdrCode == ERAR_BAD_PASSWORD || sink.passwordPrompted) {
					acc.passwordRequired = true;
					acc.error = ".rar 压缩包设有密码保护";
				} else {
					acc.error = ".rar 条目头损坏";
				}
				acc.ok = false;
				break;
			}

			if ((hdr.Flags & RHDF_DIRECTORY) != 0) {
				RARProcessFileW(hArc, RAR_SKIP, nullptr, nullptr);
				continue;
			}
			if ((hdr.Flags & RHDF_ENCRYPTED) != 0) {
				acc.passwordRequired = true;
				acc.ok = false;
				acc.error = ".rar 压缩包设有密码保护";
				break;
			}

			const std::string entryPath = (hdr.FileNameW[0] != 0)
				? wideToUtf8(hdr.FileNameW)
				: std::string(hdr.FileName);
			const uint64_t unpSize = (static_cast<uint64_t>(hdr.UnpSizeHigh) << 32) | static_cast<uint64_t>(hdr.UnpSize);

			const std::string canonical = resolveCanonicalCacheLeaf(entryPath);
			if (!canonical.empty()) {
				if (unpSize == 0 || unpSize > kMaxCacheFileBytes) {
					RARProcessFileW(hArc, RAR_SKIP, nullptr, nullptr);
					++acc.skippedFiles;
					continue;
				}
				const std::string dstPath = localCacheDir + "/" + canonical;
				const std::string tmpPath = dstPath + ".imp.tmp";
				sink.fp = std::fopen(tmpPath.c_str(), "wb");
				sink.written = 0;
				sink.maxBytes = kMaxCacheFileBytes;
				sink.writeError = (sink.fp == nullptr);

				const int procCode = RARProcessFileW(hArc, RAR_TEST, nullptr, nullptr);
				bool flushed = false;
				if (sink.fp) {
					flushed = (std::fflush(sink.fp) == 0) && !std::ferror(sink.fp);
					std::fclose(sink.fp);
					sink.fp = nullptr;
				}
				if (sink.cancelled) {
					removeFileQuiet(tmpPath);
					acc.cancelled = true;
					break;
				}
				if (sink.passwordPrompted || procCode == ERAR_MISSING_PASSWORD || procCode == ERAR_BAD_PASSWORD) {
					removeFileQuiet(tmpPath);
					acc.passwordRequired = true;
					acc.ok = false;
					acc.error = ".rar 压缩包设有密码保护";
					break;
				}
				if (procCode == ERAR_SUCCESS && !sink.writeError && flushed && sink.written > 0
					&& renameOverwrite(tmpPath, dstPath)) {
					++acc.importedFiles;
					acc.importedBytes += sink.written;
					if (onProgress && !onProgress(acc.importedFiles, acc.skippedFiles, "local/" + canonical)) {
						acc.cancelled = true;
						break;
					}
				} else {
					removeFileQuiet(tmpPath);
					if (procCode != ERAR_SUCCESS && acc.importedFiles == 0) {
						acc.ok = false;
						acc.error = ".rar 解压数据校验失败";
						break;
					}
					++acc.skippedFiles;
				}
				continue;
			}

			const std::string leaf = extractSafeEntryLeaf(entryPath);
			const NestedKind nested = (depth < kMaxArchiveNesting) ? classifyNestedArchiveLeaf(leaf) : NestedKind::None;
			if (nested != NestedKind::None && unpSize > 0 && unpSize <= kMaxNestedArchiveBytes) {
				++nestedSeq;
				char tag[64];
				std::snprintf(tag, sizeof(tag), "/.nested_%d_%d.%s.tmp", depth, nestedSeq,
					nested == NestedKind::Zip ? "zip" : "arc");
				const std::string nestedPath = localCacheDir + tag;
				const std::string tmpPath = nestedPath + ".imp.tmp";
				sink.fp = std::fopen(tmpPath.c_str(), "wb");
				sink.written = 0;
				sink.maxBytes = kMaxNestedArchiveBytes;
				sink.writeError = (sink.fp == nullptr);

				const int procCode = RARProcessFileW(hArc, RAR_TEST, nullptr, nullptr);
				bool flushed = false;
				if (sink.fp) {
					flushed = (std::fflush(sink.fp) == 0) && !std::ferror(sink.fp);
					std::fclose(sink.fp);
					sink.fp = nullptr;
				}
				if (sink.cancelled) {
					removeFileQuiet(tmpPath);
					acc.cancelled = true;
					break;
				}
				if (procCode == ERAR_SUCCESS && !sink.writeError && flushed && sink.written > 0
					&& renameOverwrite(tmpPath, nestedPath)) {
					if (nested == NestedKind::Zip) {
						acc.nestedZipPaths.push_back(nestedPath);
					} else {
						// Defer recursion until g_unrarMutex is released!
						pendingNested.push_back(PendingNestedArchive{nested, nestedPath});
					}
					continue;
				}
				removeFileQuiet(tmpPath);
			} else {
				const int skipCode = RARProcessFileW(hArc, RAR_SKIP, nullptr, nullptr);
				if (sink.cancelled) {
					acc.cancelled = true;
					break;
				}
				if (skipCode == ERAR_MISSING_PASSWORD || skipCode == ERAR_BAD_PASSWORD || sink.passwordPrompted) {
					acc.passwordRequired = true;
					acc.ok = false;
					acc.error = ".rar 压缩包设有密码保护";
					break;
				}
			}
			++acc.skippedFiles;
		}

		RARCloseArchive(hArc);
	}

	// Process any nested .7z / .rar archives outside g_unrarMutex so nested .rar never self-deadlocks!
	for (const auto& item : pendingNested) {
		if (acc.ok && !acc.cancelled) {
			extractArchiveInternal(item.tempPath, localCacheDir, depth + 1, nestedSeq, acc, onProgress);
		}
		removeFileQuiet(item.tempPath);
	}
}

void extractArchiveInternal(
	const std::string& archivePath,
	const std::string& localCacheDir,
	int depth,
	int& nestedSeq,
	CacheArchiveResult& acc,
	const CacheProgressCallback& onProgress)
{
	const ArchiveFormat fmt = detectArchiveFormat(archivePath);
	if (fmt == ArchiveFormat::SevenZip) {
		extract7zInternal(archivePath, localCacheDir, depth, nestedSeq, acc, onProgress);
	} else if (fmt == ArchiveFormat::Rar) {
		extractRarInternal(archivePath, localCacheDir, depth, nestedSeq, acc, onProgress);
	} else {
		acc.ok = false;
		acc.error = "未识别的压缩包文件头";
	}
}

} // namespace

ArchiveFormat detectArchiveFormat(const std::string& archivePath)
{
	if (archivePath.empty()) return ArchiveFormat::Unknown;
	FILE* fp = std::fopen(archivePath.c_str(), "rb");
	if (!fp) return ArchiveFormat::Unknown;
	uint8_t hdr[8]{};
	const size_t n = std::fread(hdr, 1, sizeof(hdr), fp);
	std::fclose(fp);
	if (n >= 6
		&& hdr[0] == 0x37 && hdr[1] == 0x7A
		&& hdr[2] == 0xBC && hdr[3] == 0xAF
		&& hdr[4] == 0x27 && hdr[5] == 0x1C) {
		return ArchiveFormat::SevenZip;
	}
	if (n >= 6
		&& hdr[0] == 'R' && hdr[1] == 'a' && hdr[2] == 'r' && hdr[3] == '!'
		&& hdr[4] == 0x1A && hdr[5] == 0x07) {
		return ArchiveFormat::Rar;
	}
	return ArchiveFormat::Unknown;
}

std::string resolveCanonicalCacheLeaf(const std::string& rawEntryPath)
{
	const std::string leaf = extractSafeEntryLeaf(rawEntryPath);
	if (leaf.size() < 7 || leaf.size() > 32) {
		return {};
	}
	const size_t dot = leaf.rfind('.');
	if (dot == std::string::npos || dot <= 2 || dot + 1 >= leaf.size()) {
		return {};
	}
	std::string ext = leaf.substr(dot + 1);
	for (char& ch : ext) {
		ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
	}
	if (ext != "dff" && ext != "txd") {
		return {};
	}

	const std::string stem = stripDuplicateStem(leaf.substr(0, dot));
	if (stem.size() < 3 || stem.size() > 10) {
		return {};
	}
	if (stem[0] != '0' || (stem[1] != 'x' && stem[1] != 'X')) {
		return {};
	}
	uint32_t value = 0;
	for (size_t i = 2; i < stem.size(); ++i) {
		const char ch = stem[i];
		uint32_t digit = 0;
		if (ch >= '0' && ch <= '9') {
			digit = static_cast<uint32_t>(ch - '0');
		} else if (ch >= 'a' && ch <= 'f') {
			digit = static_cast<uint32_t>(10 + (ch - 'a'));
		} else if (ch >= 'A' && ch <= 'F') {
			digit = static_cast<uint32_t>(10 + (ch - 'A'));
		} else {
			return {};
		}
		value = (value << 4) | digit;
	}
	if (value == 0u) {
		return {};
	}
	char buf[32];
	std::snprintf(buf, sizeof(buf), "0x%X.%s", value, ext.c_str());
	return std::string(buf);
}

CacheArchiveResult extractCacheArchive(
	const std::string& archivePath,
	const std::string& localCacheDir,
	const CacheProgressCallback& onProgress)
{
	CacheArchiveResult res{};
	if (archivePath.empty() || localCacheDir.empty()) {
		res.error = "无效的压缩包路径或缓存目录";
		return res;
	}
	ensureDirExists(localCacheDir);
	res.ok = true;
	int nestedSeq = 0;
	extractArchiveInternal(archivePath, localCacheDir, 0, nestedSeq, res, onProgress);
	return res;
}

} // namespace samparchive

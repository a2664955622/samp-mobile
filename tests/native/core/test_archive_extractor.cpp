// Host-side unit tests for standalone 0.3.DL cache archive extractor (.7z & .rar):
// - Real LZMA2 .7z extraction with UTF-16LE Chinese wrapper directories & duplicate suffixes
// - Real RAR5 extraction with UTF-8 Chinese + <ip:port> paths, Zip-Slip rejection, Modloader skip,
//   and nested .7z-inside-.rar extraction
// - Encrypted RAR5 header detection (passwordRequired == true without blocking)
#include "check.h"

#include "Archive/ArchiveExtractor.h"
#include "Game/Artwork/ArtworkCore.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace samparchive;

namespace {

// Real 319-byte LZMA2-compressed .7z archive containing:
//   - 中文离线包/114.66.17.73.7777/0x99bae090 (1).DFF  (120B: "RW_DFF_PAYLOAD_" * 8)
//   - 中文离线包/114.66.17.73.7777/0x0012ab.txd        (90B:  "RW_TXD_PAYLOAD_" * 6)
//   - 中文离线包/114.66.17.73.7777/infernus.dff        (72B:  must be skipped)
const uint8_t kSampleLzma2SevenZip[] = {
	0x37,0x7A,0xBC,0xAF,0x27,0x1C,0x00,0x03,0x75,0x35,0x3F,0x23,0x03,0x01,0x00,0x00,
	0x00,0x00,0x00,0x00,0x1C,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x6B,0xCD,0x4B,0x20,
	0xE0,0x01,0x19,0x00,0x29,0x5D,0x00,0x29,0x15,0xC8,0x43,0xEB,0x08,0x5E,0x27,0xCA,
	0x33,0x0A,0xFE,0x0F,0x42,0x72,0x63,0x86,0x6E,0x86,0xFF,0xAC,0xB3,0xF3,0xC2,0xE0,
	0x55,0xF8,0x9B,0x4D,0xC1,0xE1,0xB2,0x58,0x3F,0x5C,0xC5,0xF7,0x4B,0x12,0x40,0x00,
	0x00,0xE0,0x01,0xF7,0x00,0xCA,0x5D,0x00,0x00,0x81,0x33,0x07,0xAE,0x0F,0xD1,0x0F,
	0xF6,0xB8,0xA0,0x90,0xA0,0x77,0x5E,0xBA,0x58,0x3E,0x12,0xD2,0xAA,0x87,0x58,0x74,
	0x2A,0x91,0xA5,0x32,0x4D,0xB1,0x42,0x41,0x54,0xC4,0x6E,0xDC,0x20,0xC3,0x5A,0x78,
	0x0A,0xAF,0x77,0xB8,0xB0,0x40,0xF9,0x77,0x0F,0x34,0x44,0x9A,0xE3,0x6B,0x49,0x71,
	0xB3,0x83,0x36,0xB0,0xD0,0x85,0xE6,0x3E,0x86,0x56,0x04,0xCB,0x49,0xA1,0x58,0x89,
	0x04,0x21,0xCB,0xF0,0x49,0xB3,0x98,0xFE,0xE6,0x93,0xF5,0x43,0xCF,0xC2,0xEA,0xCF,
	0x7B,0x39,0xE5,0x59,0x50,0xC0,0xC1,0x75,0xB0,0x9D,0xB4,0x00,0xC2,0x3B,0x23,0x9B,
	0x71,0x1A,0x86,0x0F,0xFA,0x39,0x11,0x4C,0xBC,0x1B,0x64,0x88,0x3D,0x7B,0xAC,0x8F,
	0xAD,0x17,0xA9,0x08,0x51,0x38,0x22,0x99,0x49,0xBE,0x91,0x1E,0xF3,0xE0,0x43,0x74,
	0x9F,0x55,0x65,0xEE,0x84,0x2A,0x72,0xB5,0xA2,0xCA,0x66,0xB9,0x9F,0x3F,0x29,0x8C,
	0xFE,0xBB,0xB6,0x78,0xFC,0x59,0x4A,0x0B,0x7E,0xBE,0xA0,0xF4,0xEE,0x68,0xC4,0xFA,
	0x18,0xFC,0xB9,0xB1,0x9C,0x2F,0x27,0xD6,0x9E,0x85,0x9E,0x69,0x30,0xBF,0xA7,0x85,
	0x09,0x42,0xF5,0x35,0xF1,0x43,0x1E,0x91,0x9B,0x18,0x37,0x25,0x9D,0x69,0x3C,0x00,
	0x00,0x00,0x00,0x17,0x06,0x31,0x01,0x09,0x80,0xD2,0x00,0x07,0x0B,0x01,0x00,0x01,
	0x21,0x21,0x01,0x16,0x0C,0x81,0xF8,0x0A,0x01,0x69,0xF1,0x15,0x64,0x00,0x00
};

void pushVint(std::vector<uint8_t>& out, uint64_t v)
{
	do {
		uint8_t byte = static_cast<uint8_t>(v & 0x7Fu);
		v >>= 7;
		if (v != 0) {
			byte |= 0x80u;
		}
		out.push_back(byte);
	} while (v != 0);
}

void pushLE32(std::vector<uint8_t>& out, uint32_t v)
{
	out.push_back(static_cast<uint8_t>(v & 0xFFu));
	out.push_back(static_cast<uint8_t>((v >> 8) & 0xFFu));
	out.push_back(static_cast<uint8_t>((v >> 16) & 0xFFu));
	out.push_back(static_cast<uint8_t>((v >> 24) & 0xFFu));
}

void appendRar5Block(std::vector<uint8_t>& archive, const std::vector<uint8_t>& body, const std::vector<uint8_t>* dataArea = nullptr)
{
	std::vector<uint8_t> crcRegion;
	pushVint(crcRegion, body.size());
	crcRegion.insert(crcRegion.end(), body.begin(), body.end());
	const uint32_t crc = artwork::crc32Bytes(crcRegion.data(), crcRegion.size());
	pushLE32(archive, crc);
	archive.insert(archive.end(), crcRegion.begin(), crcRegion.end());
	if (dataArea && !dataArea->empty()) {
		archive.insert(archive.end(), dataArea->begin(), dataArea->end());
	}
}

void appendRar5StoredFile(std::vector<uint8_t>& archive, const std::string& utf8Path, const std::vector<uint8_t>& content)
{
	std::vector<uint8_t> body;
	pushVint(body, 2u);                   // HEAD_FILE = 2
	pushVint(body, 0x0002u);              // HFL_DATA = 0x0002
	pushVint(body, content.size());       // DataSize
	pushVint(body, 0x0004u);              // FHFL_CRC32 = 0x0004
	pushVint(body, content.size());       // UnpSize
	pushVint(body, 0x20u);                // FileAttr
	pushLE32(body, artwork::crc32Bytes(content.data(), content.size()));
	pushVint(body, 0u);                   // CompInfo: Method=0 (Store), UnpVer=0 (RAR5)
	pushVint(body, 0u);                   // HostOS = Windows
	pushVint(body, utf8Path.size());      // NameSize
	body.insert(body.end(), utf8Path.begin(), utf8Path.end());
	appendRar5Block(archive, body, &content);
}

std::string readFileString(const std::filesystem::path& p)
{
	std::ifstream ifs(p, std::ios::binary);
	return std::string((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
}

} // namespace

static void testCanonicalCacheLeafRules()
{
	CHECK(resolveCanonicalCacheLeaf("中文目录/114.66.17.73:7777/0x0099bae0 (1).DFF") == "0x99BAE0.dff");
	CHECK(resolveCanonicalCacheLeaf("GTA San Andreas User Files\\SAMP\\cache\\Play.Server.COM.7777\\0xcafebabe.TXD")
		== "0xCAFEBABE.txd");
	CHECK(resolveCanonicalCacheLeaf("../114.66.17.73.7777/0x1234.dff").empty());
	CHECK(resolveCanonicalCacheLeaf("cache/../../0x1234.dff").empty());
	CHECK(resolveCanonicalCacheLeaf("cache/infernus.dff").empty());
	CHECK(resolveCanonicalCacheLeaf("cache/0x0.dff").empty());
}

static void testExtractLzma2SevenZipArchive()
{
	namespace fs = std::filesystem;
	const fs::path dir = fs::temp_directory_path() / "samp_7z_extract_test";
	std::error_code ec;
	fs::remove_all(dir, ec);
	fs::create_directories(dir);

	const fs::path arcPath = dir / "中文缓存包.7z";
	{
		std::ofstream ofs(arcPath, std::ios::binary);
		ofs.write(reinterpret_cast<const char*>(kSampleLzma2SevenZip), sizeof(kSampleLzma2SevenZip));
	}
	const fs::path outLocal = dir / "local";

	CHECK(detectArchiveFormat(arcPath.generic_string()) == ArchiveFormat::SevenZip);
	CacheArchiveResult res = extractCacheArchive(arcPath.generic_string(), outLocal.generic_string());
	CHECK(res.ok);
	CHECK(!res.cancelled);
	CHECK(!res.passwordRequired);
	CHECK_EQ(res.importedFiles, 2);
	CHECK_EQ(res.skippedFiles, 1); // infernus.dff skipped
	CHECK_EQ(res.importedBytes, 120u + 90u);

	std::string expDff;
	for (int i = 0; i < 8; ++i) expDff += "RW_DFF_PAYLOAD_";
	std::string expTxd;
	for (int i = 0; i < 6; ++i) expTxd += "RW_TXD_PAYLOAD_";

	CHECK(readFileString(outLocal / "0x99BAE090.dff") == expDff);
	CHECK(readFileString(outLocal / "0x12AB.txd") == expTxd);
	CHECK(!fs::exists(outLocal / "infernus.dff"));

	fs::remove_all(dir, ec);
}

static void testExtractRar5WithChineseColonPathAndNested7z()
{
	namespace fs = std::filesystem;
	const fs::path dir = fs::temp_directory_path() / "samp_rar5_extract_test";
	std::error_code ec;
	fs::remove_all(dir, ec);
	fs::create_directories(dir);

	std::vector<uint8_t> rar5 = {'R', 'a', 'r', '!', 0x1A, 0x07, 0x01, 0x00};
	// Main archive header (HEAD_MAIN = 1, Flags = 0, ArcFlags = 0)
	appendRar5Block(rar5, {0x01, 0x00, 0x00});

	const std::string rarPayloadStr = "RAR5_DIRECT_DFF_DATA_1234567890";
	const std::vector<uint8_t> rarPayload(rarPayloadStr.begin(), rarPayloadStr.end());
	// RAR5 specification always stores '/' as universal path separator (even on HostOS=Windows)
	appendRar5StoredFile(rar5, "某服RAR缓存/114.66.17.73:7777/0xdeadbeef (2).DFF", rarPayload);

	// Zip-Slip & Modloader non-0x entries must be skipped safely
	appendRar5StoredFile(rar5, "某服RAR缓存/../../0x11111111.dff", rarPayload);
	appendRar5StoredFile(rar5, "某服RAR缓存/114.66.17.73:7777/skin.dff", rarPayload);

	// Nested .7z inside .rar!
	const std::vector<uint8_t> nested7z(
		kSampleLzma2SevenZip, kSampleLzma2SevenZip + sizeof(kSampleLzma2SevenZip));
	appendRar5StoredFile(rar5, "某服RAR缓存/套娃目录/inner_pack.7z", nested7z);

	// End of archive header (HEAD_ENDARC = 5, Flags = 0, EndArcFlags = 0)
	appendRar5Block(rar5, {0x05, 0x00, 0x00});

	const fs::path rarPath = dir / "服务器模型包.rar";
	{
		std::ofstream ofs(rarPath, std::ios::binary);
		ofs.write(reinterpret_cast<const char*>(rar5.data()), static_cast<std::streamsize>(rar5.size()));
	}

	const fs::path outLocal = dir / "local";
	CHECK(detectArchiveFormat(rarPath.generic_string()) == ArchiveFormat::Rar);
	CacheArchiveResult res = extractCacheArchive(rarPath.generic_string(), outLocal.generic_string());
	CHECK(res.ok);
	CHECK(!res.passwordRequired);
	// 1 direct (0xDEADBEEF.dff) + 2 from nested .7z (0x99BAE090.dff + 0x12AB.txd) = 3!
	CHECK_EQ(res.importedFiles, 3);
	CHECK_EQ(res.skippedFiles, 3); // Zip-Slip + skin.dff + infernus.dff inside nested .7z
	CHECK_EQ(res.importedBytes, rarPayload.size() + 120u + 90u);

	CHECK(readFileString(outLocal / "0xDEADBEEF.dff") == rarPayloadStr);
	CHECK(fs::exists(outLocal / "0x99BAE090.dff"));
	CHECK(fs::exists(outLocal / "0x12AB.txd"));
	CHECK(!fs::exists(outLocal / "0x11111111.dff"));
	CHECK(!fs::exists(outLocal / "skin.dff"));

	fs::remove_all(dir, ec);
}

static void testEncryptedRar5ReportsPasswordRequiredWithoutBlocking()
{
	namespace fs = std::filesystem;
	const fs::path dir = fs::temp_directory_path() / "samp_rar5_enc_test";
	std::error_code ec;
	fs::remove_all(dir, ec);
	fs::create_directories(dir);

	std::vector<uint8_t> encRar = {'R', 'a', 'r', '!', 0x1A, 0x07, 0x01, 0x00};
	// Archive encryption header (HEAD_CRYPT = 4, Flags = 0, CryptVer = 0, EncFlags = 0, Lg2Count = 15, 16B salt)
	std::vector<uint8_t> cryptBody = {0x04, 0x00, 0x00, 0x00, 15};
	cryptBody.insert(cryptBody.end(), 16, 0x5Au);
	appendRar5Block(encRar, cryptBody);
	// Trailing encrypted IV + dummy ciphertext bytes
	encRar.insert(encRar.end(), 48, 0xA5u);

	const fs::path rarPath = dir / "locked.rar";
	{
		std::ofstream ofs(rarPath, std::ios::binary);
		ofs.write(reinterpret_cast<const char*>(encRar.data()), static_cast<std::streamsize>(encRar.size()));
	}

	const fs::path outLocal = dir / "local";
	CacheArchiveResult res = extractCacheArchive(rarPath.generic_string(), outLocal.generic_string());
	CHECK(!res.ok);
	CHECK(res.passwordRequired);
	CHECK_EQ(res.importedFiles, 0);

	fs::remove_all(dir, ec);
}

int main()
{
	testCanonicalCacheLeafRules();
	testExtractLzma2SevenZipArchive();
	testExtractRar5WithChineseColonPathAndNested7z();
	testEncryptedRar5ReportsPasswordRequiredWithoutBlocking();
	return check::finish();
}

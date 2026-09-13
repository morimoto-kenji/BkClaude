#include <windows.h>
#include <wincrypt.h>
#include <fstream>
#include <cstdio>
#include "Settings.h"
#include "Encoding.h"
#include "BeckyApi.h"

extern CBeckyAPI bka;

namespace {

std::string JoinPath(const std::string& dir, const std::string& name)
{
	std::string path = dir;
	if (!path.empty() && path.back() != '\\') {
		path += '\\';
	}
	path += name;
	return path;
}

// CryptProtectData ties the ciphertext to this Windows user account, so
// apikey.bin is useless if copied to another machine or user profile.
const wchar_t* kApiKeyDescription = L"BkClaude API Key";

} // namespace

std::string GetPluginDataDir()
{
	std::string dir = JoinPath(bka.GetDataFolder(), "BkClaude");
	CreateDirectoryA(dir.c_str(), NULL);
	return dir;
}

bool LoadSettings(PluginSettings& out)
{
	std::string iniPath = JoinPath(GetPluginDataDir(), "config.ini");

	char buf[256];
	GetPrivateProfileStringA("Settings", "Model", "claude-haiku-4-5", buf, sizeof(buf), iniPath.c_str());
	out.model = buf;

	out.summarizeCacheMinutes = GetPrivateProfileIntA("Settings", "SummarizeCacheMinutes", 1440, iniPath.c_str());
	out.translateCacheMinutes = GetPrivateProfileIntA("Settings", "TranslateCacheMinutes", 1440, iniPath.c_str());

	out.learnMaxMails = GetPrivateProfileIntA("Settings", "LearnMaxMails", 200, iniPath.c_str());
	out.learnMaxMonths = GetPrivateProfileIntA("Settings", "LearnMaxMonths", 0, iniPath.c_str());

	out.warnMissingAttachment =
		GetPrivateProfileIntA("Settings", "WarnMissingAttachment", 1, iniPath.c_str()) != 0;

	GetPrivateProfileStringA("Settings", "LicenseKey", "", buf, sizeof(buf), iniPath.c_str());
	out.licenseKey = buf;

	return true;
}

bool SaveSettings(const PluginSettings& in)
{
	std::string iniPath = JoinPath(GetPluginDataDir(), "config.ini");
	WritePrivateProfileStringA("Settings", "Model", in.model.c_str(), iniPath.c_str());
	char cacheBuf[16];
	sprintf(cacheBuf, "%d", in.summarizeCacheMinutes);
	WritePrivateProfileStringA("Settings", "SummarizeCacheMinutes", cacheBuf, iniPath.c_str());
	sprintf(cacheBuf, "%d", in.translateCacheMinutes);
	WritePrivateProfileStringA("Settings", "TranslateCacheMinutes", cacheBuf, iniPath.c_str());
	sprintf(cacheBuf, "%d", in.learnMaxMails);
	WritePrivateProfileStringA("Settings", "LearnMaxMails", cacheBuf, iniPath.c_str());
	sprintf(cacheBuf, "%d", in.learnMaxMonths);
	WritePrivateProfileStringA("Settings", "LearnMaxMonths", cacheBuf, iniPath.c_str());
	WritePrivateProfileStringA("Settings", "WarnMissingAttachment",
		in.warnMissingAttachment ? "1" : "0", iniPath.c_str());
	WritePrivateProfileStringA("Settings", "LicenseKey",
		in.licenseKey.empty() ? NULL : in.licenseKey.c_str(), iniPath.c_str());
	return true;
}

bool IsLicenseKeyValid(const std::string& key)
{
	// Forgiving on whitespace and case: the point is that everyone who types
	// it gets the 登録済み line, and nobody is turned away on a technicality.
	size_t a = key.find_first_not_of(" \t\r\n");
	if (a == std::string::npos) return false;
	size_t b = key.find_last_not_of(" \t\r\n");
	std::string k = key.substr(a, b - a + 1);
	for (char& c : k) {
		if (c >= 'a' && c <= 'z') c -= ('a' - 'A');
	}
	return k == "MORIMOTO=4000YEN";
}

bool HasApiKey()
{
	std::string path = JoinPath(GetPluginDataDir(), "apikey.bin");
	std::ifstream f(path, std::ios::binary);
	return f.good() && f.peek() != std::ifstream::traits_type::eof();
}

bool LoadApiKey(std::string& outKey)
{
	std::string path = JoinPath(GetPluginDataDir(), "apikey.bin");
	std::ifstream f(path, std::ios::binary);
	if (!f.good()) {
		return false;
	}
	std::string cipher((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
	if (cipher.empty()) {
		return false;
	}

	DATA_BLOB in;
	in.pbData = (BYTE*)cipher.data();
	in.cbData = (DWORD)cipher.size();
	DATA_BLOB out{};
	if (!CryptUnprotectData(&in, NULL, NULL, NULL, NULL, CRYPTPROTECT_UI_FORBIDDEN, &out)) {
		return false;
	}
	outKey.assign((char*)out.pbData, out.cbData);
	LocalFree(out.pbData);
	return true;
}

std::string MailboxIdFromFolderId(const std::string& folderId)
{
	size_t p = folderId.find(".mb\\");
	if (p == std::string::npos) return folderId;
	return folderId.substr(0, p + 4); // keep the trailing ".mb\"
}

namespace {

// Mailbox ids contain '\' and '.', neither of which belongs in a file name
// or an INI key.
std::string SanitizeId(const std::string& mailboxId)
{
	std::string out;
	for (char c : mailboxId) {
		out += (c == '\\' || c == '/' || c == ':' || c == '.') ? '_' : c;
	}
	return out;
}

} // namespace

std::vector<SentFolderInfo> LoadSentFolders()
{
	std::string iniPath = JoinPath(GetPluginDataDir(), "config.ini");
	std::vector<SentFolderInfo> folders;

	int count = GetPrivateProfileIntA("SentFolders", "Count", 0, iniPath.c_str());
	for (int i = 0; i < count; i++) {
		char key[32];
		sprintf(key, "Folder%d", i);
		char folderBuf[1024];
		GetPrivateProfileStringA("SentFolders", key, "", folderBuf, sizeof(folderBuf), iniPath.c_str());
		if (!folderBuf[0]) continue;

		SentFolderInfo info;
		info.folderId = folderBuf;

		std::string learnedKey = SanitizeId(MailboxIdFromFolderId(info.folderId));
		char learnedBuf[64];
		GetPrivateProfileStringA("Learned", learnedKey.c_str(), "", learnedBuf, sizeof(learnedBuf), iniPath.c_str());
		info.learnedAt = learnedBuf;
		std::string countKey = learnedKey + "_n";
		info.sampleCount = GetPrivateProfileIntA("Learned", countKey.c_str(), 0, iniPath.c_str());
		std::string modelKey = learnedKey + "_model";
		char modelBuf[128];
		GetPrivateProfileStringA("Learned", modelKey.c_str(), "", modelBuf, sizeof(modelBuf), iniPath.c_str());
		info.learnedModel = modelBuf;

		folders.push_back(info);
	}
	return folders;
}

void SaveSentFolders(const std::vector<SentFolderInfo>& folders)
{
	std::string iniPath = JoinPath(GetPluginDataDir(), "config.ini");

	// Drop both sections first so removed entries can't linger. [Learned] is
	// derived entirely from this list, and a stale stamp there would make a
	// re-registered mailbox claim a profile that is no longer on disk.
	WritePrivateProfileStringA("SentFolders", NULL, NULL, iniPath.c_str());
	WritePrivateProfileStringA("Learned", NULL, NULL, iniPath.c_str());

	char buf[32];
	sprintf(buf, "%d", (int)folders.size());
	WritePrivateProfileStringA("SentFolders", "Count", buf, iniPath.c_str());

	for (size_t i = 0; i < folders.size(); i++) {
		char key[32];
		sprintf(key, "Folder%d", (int)i);
		WritePrivateProfileStringA("SentFolders", key, folders[i].folderId.c_str(), iniPath.c_str());

		std::string learnedKey = SanitizeId(MailboxIdFromFolderId(folders[i].folderId));
		WritePrivateProfileStringA("Learned", learnedKey.c_str(),
			folders[i].learnedAt.empty() ? NULL : folders[i].learnedAt.c_str(), iniPath.c_str());
		std::string countKey = learnedKey + "_n";
		sprintf(buf, "%d", folders[i].sampleCount);
		WritePrivateProfileStringA("Learned", countKey.c_str(),
			folders[i].sampleCount > 0 ? buf : NULL, iniPath.c_str());
		std::string modelKey = learnedKey + "_model";
		WritePrivateProfileStringA("Learned", modelKey.c_str(),
			folders[i].learnedModel.empty() ? NULL : folders[i].learnedModel.c_str(), iniPath.c_str());
	}
}

bool LoadStyleProfile(const std::string& mailboxId, std::string& outUtf8Text)
{
	std::string path = JoinPath(GetPluginDataDir(), "style_" + SanitizeId(mailboxId) + ".txt");
	std::ifstream f(path, std::ios::binary);
	if (!f.good()) {
		return false;
	}
	outUtf8Text.assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
	return !outUtf8Text.empty();
}

bool SaveStyleProfile(const std::string& mailboxId, const std::string& utf8Text)
{
	std::string path = JoinPath(GetPluginDataDir(), "style_" + SanitizeId(mailboxId) + ".txt");
	std::ofstream f(path, std::ios::binary | std::ios::trunc);
	if (!f.good()) {
		return false;
	}
	f.write(utf8Text.data(), (std::streamsize)utf8Text.size());
	return f.good();
}

namespace {

std::string ToLowerAscii(std::string s)
{
	for (char& c : s) {
		if (c >= 'A' && c <= 'Z') c += ('a' - 'A');
	}
	return s;
}

std::string ReadWholeFile(const std::string& path)
{
	std::ifstream f(path, std::ios::binary);
	if (!f.good()) return std::string();
	return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// Signature files from one mailbox folder. The ".txt.utf8" twin is preferred
// where it exists; the plain ".txt" is in the system codepage and is only
// used for the entries that have no twin.
void CollectSignaturesIn(const std::string& mailboxDir, std::vector<std::string>& out)
{
	std::vector<std::string> utf8Bases;
	std::vector<std::string> ansiFiles;

	WIN32_FIND_DATAA fd;
	HANDLE h = FindFirstFileA((mailboxDir + "\\$$$*").c_str(), &fd);
	if (h == INVALID_HANDLE_VALUE) return;
	do {
		if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
		std::string name = fd.cFileName;
		const std::string kUtf8 = ".txt.utf8";
		if (name.size() > kUtf8.size() &&
		    name.compare(name.size() - kUtf8.size(), kUtf8.size(), kUtf8) == 0) {
			utf8Bases.push_back(name.substr(0, name.size() - kUtf8.size()));
			out.push_back(ReadWholeFile(mailboxDir + "\\" + name));
		} else {
			ansiFiles.push_back(name);
		}
	} while (FindNextFileA(h, &fd));
	FindClose(h);

	for (const std::string& name : ansiFiles) {
		const std::string kTxt = ".txt";
		if (name.size() <= kTxt.size() ||
		    name.compare(name.size() - kTxt.size(), kTxt.size(), kTxt) != 0) continue;
		std::string base = name.substr(0, name.size() - kTxt.size());
		bool hasUtf8Twin = false;
		for (const std::string& b : utf8Bases) {
			if (b == base) { hasUtf8Twin = true; break; }
		}
		if (hasUtf8Twin) continue;
		out.push_back(AnsiToUtf8(ReadWholeFile(mailboxDir + "\\" + name)));
	}
}

} // namespace

std::vector<std::string> LoadAllSignatures()
{
	std::vector<std::string> signatures;

	LPCTSTR lpData = bka.GetDataFolder();
	if (!lpData || !*lpData) return signatures;
	std::string dataDir = lpData;
	if (!dataDir.empty() && dataDir.back() == '\\') dataDir.pop_back();

	WIN32_FIND_DATAA fd;
	HANDLE h = FindFirstFileA((dataDir + "\\*.mb").c_str(), &fd);
	if (h == INVALID_HANDLE_VALUE) return signatures;
	do {
		if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
		CollectSignaturesIn(dataDir + "\\" + fd.cFileName, signatures);
	} while (FindNextFileA(h, &fd));
	FindClose(h);

	return signatures;
}

std::string MailboxIdForAddress(const std::string& address)
{
	if (address.empty()) return std::string();
	std::string wanted = ToLowerAscii(address);

	LPCTSTR lpData = bka.GetDataFolder();
	if (!lpData || !*lpData) return std::string();
	std::string dataDir = lpData;
	if (!dataDir.empty() && dataDir.back() == '\\') dataDir.pop_back();

	std::string found;
	WIN32_FIND_DATAA fd;
	HANDLE h = FindFirstFileA((dataDir + "\\*.mb").c_str(), &fd);
	if (h == INVALID_HANDLE_VALUE) return std::string();
	do {
		if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
		std::string ini = dataDir + "\\" + fd.cFileName + "\\Mailbox.ini";
		char buf[256];
		GetPrivateProfileStringA("Account", "MailAddress", "", buf, sizeof(buf), ini.c_str());
		if (buf[0] && ToLowerAscii(buf) == wanted) {
			found = std::string(fd.cFileName) + "\\";
			break;
		}
	} while (FindNextFileA(h, &fd));
	FindClose(h);
	return found;
}

bool DeleteStyleProfile(const std::string& mailboxId)
{
	std::string path = JoinPath(GetPluginDataDir(), "style_" + SanitizeId(mailboxId) + ".txt");
	// Nothing to delete counts as success: the caller's goal is "no profile".
	if (GetFileAttributesA(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
		return true;
	}
	return DeleteFileA(path.c_str()) != 0;
}

namespace {

std::string AttachPhrasePath(const std::string& mailboxId)
{
	return JoinPath(GetPluginDataDir(), "attach_" + SanitizeId(mailboxId) + ".txt");
}

} // namespace

bool LoadAttachPhrases(const std::string& mailboxId, std::vector<std::string>& outPhrases)
{
	outPhrases.clear();

	std::ifstream f(AttachPhrasePath(mailboxId), std::ios::binary);
	if (!f.good()) return false;
	std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());

	size_t pos = 0;
	while (pos <= text.size()) {
		size_t eol = text.find('\n', pos);
		size_t end = (eol == std::string::npos) ? text.size() : eol;
		std::string line = text.substr(pos, end - pos);

		// Hand-edited files get CR endings and stray spaces; '#' opens a
		// comment so a phrase can be disabled without losing what it was.
		while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) {
			line.pop_back();
		}
		size_t first = line.find_first_not_of(" \t");
		if (first != std::string::npos && line[first] != '#') {
			outPhrases.push_back(line.substr(first));
		}

		if (eol == std::string::npos) break;
		pos = eol + 1;
	}
	return !outPhrases.empty();
}

bool SaveAttachPhrases(const std::string& mailboxId, const std::string& utf8Lines)
{
	std::ofstream f(AttachPhrasePath(mailboxId), std::ios::binary | std::ios::trunc);
	if (!f.good()) return false;
	f.write(utf8Lines.data(), (std::streamsize)utf8Lines.size());
	return f.good();
}

bool DeleteAttachPhrases(const std::string& mailboxId)
{
	std::string path = AttachPhrasePath(mailboxId);
	if (GetFileAttributesA(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
		return true;
	}
	return DeleteFileA(path.c_str()) != 0;
}

bool SaveApiKey(const std::string& key)
{
	DATA_BLOB in;
	in.pbData = (BYTE*)key.data();
	in.cbData = (DWORD)key.size();
	DATA_BLOB out{};
	if (!CryptProtectData(&in, kApiKeyDescription, NULL, NULL, NULL, CRYPTPROTECT_UI_FORBIDDEN, &out)) {
		return false;
	}

	std::string path = JoinPath(GetPluginDataDir(), "apikey.bin");
	std::ofstream f(path, std::ios::binary | std::ios::trunc);
	bool ok = false;
	if (f.good()) {
		f.write((char*)out.pbData, out.cbData);
		ok = f.good();
	}
	LocalFree(out.pbData);
	return ok;
}

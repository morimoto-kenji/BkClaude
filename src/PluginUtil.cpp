#include <windows.h>
#include <string>
#include <vector>
#include "PluginUtil.h"
#include "BeckyApi.h"
#include "Settings.h"
#include "Encoding.h"

extern CBeckyAPI bka;

std::string TrimBlank(const std::string& s)
{
	size_t a = s.find_first_not_of(" \t\r\n");
	if (a == std::string::npos) return std::string();
	size_t b = s.find_last_not_of(" \t\r\n");
	return s.substr(a, b - a + 1);
}

// Cuts a UTF-8 string to at most maxBytes without splitting a multi-byte
// character. A split character is invalid UTF-8, which makes the JSON
// encoder throw - and an escaping exception aborts the whole host process.
std::string TruncateUtf8(const std::string& s, size_t maxBytes)
{
	if (s.size() <= maxBytes) return s;
	size_t cut = maxBytes;
	// UTF-8 continuation bytes are 10xxxxxx; step back off them to a lead byte.
	while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) {
		cut--;
	}
	return s.substr(0, cut);
}

bool RequireApiKey(std::string& outKey, std::string& outModel)
{
	if (!LoadApiKey(outKey) || outKey.empty()) {
		MessageBoxW(NULL,
			L"APIキーが未設定です。「ツール」→「プラグインの設定」から設定してください。",
			L"Becky! Claude Plugin", MB_OK | MB_ICONWARNING);
		return false;
	}
	PluginSettings settings;
	LoadSettings(settings);
	outModel = settings.model;
	return true;
}

// The recipient addresses from a To header, lowercased, at most `maxCount`.
// Only the addresses - display names are MIME-encoded and, more to the point,
// the address is what the draft feature will have in hand to match against.
std::string ExtractAddresses(const std::string& header, size_t maxCount)
{
	std::vector<std::string> addrs;

	size_t p = 0;
	while ((p = header.find('<', p)) != std::string::npos) {
		size_t e = header.find('>', p);
		if (e == std::string::npos) break;
		std::string a = header.substr(p + 1, e - p - 1);
		if (a.find('@') != std::string::npos) addrs.push_back(a);
		p = e + 1;
	}
	if (addrs.empty()) {
		// Bare "user@host" form, no angle brackets.
		size_t start = 0;
		while (start <= header.size()) {
			size_t sep = header.find_first_of(", \t\r\n", start);
			size_t end = (sep == std::string::npos) ? header.size() : sep;
			std::string tok = header.substr(start, end - start);
			if (tok.find('@') != std::string::npos) addrs.push_back(tok);
			if (sep == std::string::npos) break;
			start = sep + 1;
		}
	}

	std::string out;
	size_t used = 0;
	for (std::string& a : addrs) {
		if (used >= maxCount) break;
		for (char& c : a) if (c >= 'A' && c <= 'Z') c += ('a' - 'A');
		if (!out.empty()) out += ", ";
		out += a;
		used++;
	}
	if (addrs.size() > used) out += ", ...";
	return out;
}

// Drops lines that look like quoted history ('>' prefix) so the style
// corpus reflects only what the user actually wrote, not quoted replies.
std::string StripQuotedLines(const std::string& utf8Text)
{
	std::string out;
	size_t start = 0;
	while (start <= utf8Text.size()) {
		size_t nl = utf8Text.find('\n', start);
		size_t end = (nl == std::string::npos) ? utf8Text.size() : nl;
		std::string line = utf8Text.substr(start, end - start);
		size_t firstNonSpace = line.find_first_not_of(" \t\r");
		bool isQuoted = firstNonSpace != std::string::npos && line[firstNonSpace] == '>';
		if (!isQuoted) {
			out += line;
			out += '\n';
		}
		if (nl == std::string::npos) break;
		start = nl + 1;
	}
	return out;
}

std::string ToLowerAscii(std::string s)
{
	for (char& c : s) {
		if (c >= 'A' && c <= 'Z') c += ('a' - 'A');
	}
	return s;
}

// One header field of the current mail, MIME-decoded (a no-op when the field
// has no encoded-words) and converted to UTF-8. Becky! decodes headers into
// the system codepage, which is why this doesn't use the mail's own charset.
std::string GetHeaderUtf8(const char* name)
{
	char raw[2048] = {};
	bka.GetSpecifiedHeader(name, raw, sizeof(raw));
	if (!raw[0]) return std::string();

	char charsetName[64] = {};
	LPSTR decoded = bka.MIMEHeader(raw, charsetName, sizeof(charsetName), FALSE);
	std::string value = (decoded && *decoded) ? decoded : raw;

	// To/Cc can span several CRLF-separated lines; flatten for the prompt.
	for (char& c : value) {
		if (c == '\r' || c == '\n') c = ' ';
	}
	return AnsiToUtf8(value);
}

// Same, for the message being composed rather than the current mail. To/Cc
// can be long, so the buffer is generous.
std::string CompGetHeaderUtf8(HWND hWnd, const char* name)
{
	char raw[4096] = {};
	bka.CompGetSpecifiedHeader(hWnd, name, raw, sizeof(raw));
	if (!raw[0]) return std::string();

	char charsetName[64] = {};
	LPSTR decoded = bka.MIMEHeader(raw, charsetName, sizeof(charsetName), FALSE);
	std::string value = (decoded && *decoded) ? decoded : raw;
	for (char& c : value) {
		if (c == '\r' || c == '\n') c = ' ';
	}
	return AnsiToUtf8(value);
}

// Which mailbox's learned style applies to this compose window. X-Becky-Ref
// names the mail being replied to - a folder id with '>' before the serial -
// so it identifies the account directly. Falls back to whatever folder the
// main window is showing, which is where the original normally sits.
std::string MailboxIdForCompose(HWND hWnd)
{
	char ref[1024] = {};
	bka.CompGetSpecifiedHeader(hWnd, "X-Becky-Ref", ref, sizeof(ref));
	if (ref[0]) {
		std::string id = MailboxIdFromFolderId(ref);
		if (id.find(".mb\\") != std::string::npos) return id;
	}

	// A new mail refers to no existing mail, so there is no X-Becky-Ref - but
	// it does carry the sending account's own From address, which names the
	// mailbox exactly. Without this the account was guessed from whatever
	// folder the main window happened to be showing.
	std::string fromAddr = ExtractAddresses(CompGetHeaderUtf8(hWnd, "From"), 1);
	if (!fromAddr.empty()) {
		std::string id = MailboxIdForAddress(fromAddr);
		if (!id.empty()) return id;
	}

	LPCTSTR lpFolder = bka.GetCurrentFolder();
	if (!lpFolder || !*lpFolder) return std::string();
	std::string id = MailboxIdFromFolderId(lpFolder);
	return (id.find(".mb\\") != std::string::npos) ? id : std::string();
}

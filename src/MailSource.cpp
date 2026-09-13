#include <windows.h>
#include <vector>
#include <cctype>
#include "MailSource.h"
#include "Encoding.h"

namespace {

std::string ToLower(std::string s)
{
	for (char& c : s) {
		if (c >= 'A' && c <= 'Z') c += ('a' - 'A');
	}
	return s;
}

std::string Trim(const std::string& s)
{
	size_t a = s.find_first_not_of(" \t\r\n");
	if (a == std::string::npos) return std::string();
	size_t b = s.find_last_not_of(" \t\r\n");
	return s.substr(a, b - a + 1);
}

// Splits a message (or a MIME part) at the blank line separating its
// headers from its content.
void SplitHeadersBody(const std::string& src, std::string& headers, std::string& body)
{
	size_t p = src.find("\r\n\r\n");
	size_t skip = 4;
	if (p == std::string::npos) {
		p = src.find("\n\n");
		skip = 2;
	}
	if (p == std::string::npos) {
		headers = src;
		body.clear();
		return;
	}
	headers = src.substr(0, p);
	body = src.substr(p + skip);
}

// Header lookup that also unfolds continuation lines (those starting with
// space or tab), which long Content-Type headers routinely use.
std::string GetHeaderValue(const std::string& headers, const std::string& name)
{
	std::string lower = ToLower(headers);
	std::string needle = ToLower(name) + ":";

	size_t pos = 0;
	while (pos < lower.size()) {
		bool atLineStart = (pos == 0) || (lower[pos - 1] == '\n');
		if (atLineStart && lower.compare(pos, needle.size(), needle) == 0) {
			std::string value;
			size_t i = pos + needle.size();
			while (i <= headers.size()) {
				size_t lineEnd = headers.find('\n', i);
				if (lineEnd == std::string::npos) lineEnd = headers.size();
				std::string line = headers.substr(i, lineEnd - i);
				if (!line.empty() && line.back() == '\r') line.pop_back();
				value += line;
				i = lineEnd + 1;
				if (i >= headers.size()) break;
				if (headers[i] != ' ' && headers[i] != '\t') break;
				value += ' ';
			}
			return Trim(value);
		}
		size_t nl = lower.find('\n', pos);
		if (nl == std::string::npos) break;
		pos = nl + 1;
	}
	return std::string();
}

// e.g. GetParam("multipart/mixed; boundary=\"abc\"", "boundary") -> abc
std::string GetParam(const std::string& headerValue, const std::string& param)
{
	std::string lower = ToLower(headerValue);
	std::string needle = ToLower(param) + "=";
	size_t p = lower.find(needle);
	if (p == std::string::npos) return std::string();

	size_t v = p + needle.size();
	if (v < headerValue.size() && headerValue[v] == '"') {
		size_t end = headerValue.find('"', v + 1);
		if (end == std::string::npos) return std::string();
		return headerValue.substr(v + 1, end - v - 1);
	}
	size_t end = headerValue.find_first_of("; \t\r\n", v);
	if (end == std::string::npos) end = headerValue.size();
	return headerValue.substr(v, end - v);
}

std::vector<std::string> SplitMultipart(const std::string& body, const std::string& boundary)
{
	std::vector<std::string> parts;
	if (boundary.empty()) return parts;

	std::string delim = "--" + boundary;
	size_t pos = body.find(delim);
	while (pos != std::string::npos) {
		size_t lineEnd = body.find('\n', pos);
		if (lineEnd == std::string::npos) break;

		std::string delimLine = body.substr(pos, lineEnd - pos);
		bool closing = delimLine.size() >= delim.size() + 2 &&
		               delimLine.compare(delim.size(), 2, "--") == 0;
		if (closing) break;

		size_t partStart = lineEnd + 1;
		size_t next = body.find("\n" + delim, partStart);
		if (next == std::string::npos) {
			parts.push_back(body.substr(partStart));
			break;
		}
		parts.push_back(body.substr(partStart, next - partStart));
		pos = next + 1;
	}
	return parts;
}

std::string Base64Decode(const std::string& in)
{
	auto sextet = [](char c) -> int {
		if (c >= 'A' && c <= 'Z') return c - 'A';
		if (c >= 'a' && c <= 'z') return c - 'a' + 26;
		if (c >= '0' && c <= '9') return c - '0' + 52;
		if (c == '+') return 62;
		if (c == '/') return 63;
		return -1; // whitespace, line breaks, anything else
	};

	std::string out;
	int buf = 0, bits = 0;
	for (char c : in) {
		if (c == '=') break;
		int v = sextet(c);
		if (v < 0) continue;
		buf = (buf << 6) | v;
		bits += 6;
		if (bits >= 8) {
			bits -= 8;
			out += (char)((buf >> bits) & 0xFF);
		}
	}
	return out;
}

std::string QuotedPrintableDecode(const std::string& in)
{
	auto hex = [](char h) -> int {
		if (h >= '0' && h <= '9') return h - '0';
		return (h | 0x20) - 'a' + 10;
	};

	std::string out;
	for (size_t i = 0; i < in.size(); i++) {
		char c = in[i];
		if (c != '=') {
			out += c;
			continue;
		}
		if (i + 1 < in.size() && (in[i + 1] == '\r' || in[i + 1] == '\n')) {
			i += (in[i + 1] == '\r' && i + 2 < in.size() && in[i + 2] == '\n') ? 2 : 1;
			continue; // soft line break
		}
		if (i + 2 < in.size() && isxdigit((unsigned char)in[i + 1]) && isxdigit((unsigned char)in[i + 2])) {
			out += (char)((hex(in[i + 1]) << 4) | hex(in[i + 2]));
			i += 2;
			continue;
		}
		out += c;
	}
	return out;
}

UINT CodepageFromCharset(const std::string& charset)
{
	std::string cs = ToLower(Trim(charset));
	if (cs.empty()) return CP_ACP;
	if (cs.find("utf-8") != std::string::npos || cs.find("utf8") != std::string::npos) return CP_UTF8;
	if (cs.find("iso-2022-jp") != std::string::npos) return 50221;
	if (cs.find("shift_jis") != std::string::npos || cs.find("shift-jis") != std::string::npos ||
	    cs.find("sjis") != std::string::npos || cs.find("windows-31j") != std::string::npos ||
	    cs.find("cp932") != std::string::npos) return 932;
	if (cs.find("euc-jp") != std::string::npos || cs.find("eucjp") != std::string::npos) return 51932;
	if (cs.find("iso-8859-1") != std::string::npos || cs.find("latin1") != std::string::npos) return 28591;
	if (cs.find("us-ascii") != std::string::npos || cs.find("ascii") != std::string::npos) return 20127;
	return CP_ACP;
}

// Applies a part's Content-Transfer-Encoding and charset, yielding UTF-8.
std::string DecodePart(const std::string& partHeaders, const std::string& partBody)
{
	std::string cte = ToLower(GetHeaderValue(partHeaders, "Content-Transfer-Encoding"));
	std::string raw;
	if (cte.find("base64") != std::string::npos) {
		raw = Base64Decode(partBody);
	} else if (cte.find("quoted-printable") != std::string::npos) {
		raw = QuotedPrintableDecode(partBody);
	} else {
		raw = partBody; // 7bit / 8bit / binary / absent
	}

	std::string contentType = GetHeaderValue(partHeaders, "Content-Type");
	return AnsiToUtf8(raw, CodepageFromCharset(GetParam(contentType, "charset")));
}

bool IsType(const std::string& contentType, const char* wanted)
{
	return ToLower(contentType).find(wanted) != std::string::npos;
}

// A horizontal rule of repeated punctuation. Becky!'s own reply template
// draws one under its quote header using hyphens; other clients use
// underscores or equals signs.
bool IsRuleLine(const std::string& line)
{
	if (line.size() < 20) return false;
	char c = line[0];
	if (c != '-' && c != '_' && c != '=' && c != '*') return false;
	return line.find_first_not_of(c) == std::string::npos;
}

// "<name>さんは書きました" / "On <date>, <name> wrote:" and friends.
bool IsAttributionLine(const std::string& line)
{
	if (line.empty() || line.size() >= 200) return false;
	return line.find("さんは書きました") != std::string::npos ||
	       line.find("さんが書きました") != std::string::npos ||
	       (line.rfind("On ", 0) == 0 && line.find("wrote:") != std::string::npos);
}

} // namespace

std::string GetTopLevelHeader(const std::string& rawSource, const std::string& name)
{
	std::string headers, body;
	SplitHeadersBody(rawSource, headers, body);
	return GetHeaderValue(headers, name);
}

bool SourceHasAttachment(const std::string& rawSource)
{
	// A line-by-line scan of the whole source, not a parse of the MIME tree:
	// the header can sit at any nesting depth, and a false negative here is
	// the expensive direction - it is what makes the warning fire on a mail
	// that does have its file. Matching only at the start of a line keeps the
	// same words appearing inside body text from counting.
	size_t pos = 0;
	while (pos < rawSource.size()) {
		size_t eol = rawSource.find('\n', pos);
		size_t end = (eol == std::string::npos) ? rawSource.size() : eol;
		std::string line = ToLower(rawSource.substr(pos, end - pos));

		const std::string kName = "content-disposition:";
		if (line.compare(0, kName.size(), kName) == 0 &&
		    line.find("attachment", kName.size()) != std::string::npos) {
			return true;
		}
		if (eol == std::string::npos) break;
		pos = eol + 1;
	}
	return false;
}

std::string ExtractPlainTextBodyUtf8(const std::string& rawSource)
{
	std::string headers, body;
	SplitHeadersBody(rawSource, headers, body);

	std::string contentType = GetHeaderValue(headers, "Content-Type");
	if (contentType.empty()) contentType = "text/plain"; // RFC default

	if (!IsType(contentType, "multipart/")) {
		return IsType(contentType, "text/plain") ? DecodePart(headers, body) : std::string();
	}

	for (const std::string& part : SplitMultipart(body, GetParam(contentType, "boundary"))) {
		std::string pHeaders, pBody;
		SplitHeadersBody(part, pHeaders, pBody);
		std::string pType = GetHeaderValue(pHeaders, "Content-Type");
		if (pType.empty()) pType = "text/plain";

		if (IsType(pType, "text/plain")) {
			return DecodePart(pHeaders, pBody);
		}
		if (IsType(pType, "multipart/alternative")) {
			// The one descent we allow: alternative holds the same message
			// again as text and HTML, so its text/plain is still ours.
			for (const std::string& sub : SplitMultipart(pBody, GetParam(pType, "boundary"))) {
				std::string sHeaders, sBody;
				SplitHeadersBody(sub, sHeaders, sBody);
				std::string sType = GetHeaderValue(sHeaders, "Content-Type");
				if (sType.empty()) sType = "text/plain";
				if (IsType(sType, "text/plain")) {
					return DecodePart(sHeaders, sBody);
				}
			}
		}
		// message/rfc822, application/*, image/* ...: skipped, never entered.
	}
	return std::string();
}

std::string StripForwardedTail(const std::string& utf8Text)
{
	static const char* kSeparators[] = {
		"-----Original Message-----",
		"-----Original message-----",
		"----- Original Message -----",
		"-----元のメッセージ-----",
		"-------- 転送されたメッセージ",
		"-------- Forwarded Message",
	};

	size_t cut = std::string::npos;
	for (const char* sep : kSeparators) {
		size_t p = utf8Text.find(sep);
		if (p != std::string::npos && p < cut) cut = p;
	}

	// Line-shaped separators: a horizontal rule, or an attribution line. A
	// rule underlines a quote header, so cut at the header above it.
	size_t start = 0;
	size_t prevStart = std::string::npos;
	while (start <= utf8Text.size() && start < cut) {
		size_t nl = utf8Text.find('\n', start);
		size_t end = (nl == std::string::npos) ? utf8Text.size() : nl;
		std::string line = Trim(utf8Text.substr(start, end - start));

		if (IsAttributionLine(line)) {
			cut = start;
			break;
		}
		if (IsRuleLine(line)) {
			cut = (prevStart != std::string::npos) ? prevStart : start;
			break;
		}

		prevStart = line.empty() ? std::string::npos : start;
		if (nl == std::string::npos) break;
		start = nl + 1;
	}

	if (cut == std::string::npos) return utf8Text;
	return utf8Text.substr(0, cut);
}

namespace {

// Offset of a configured signature inside the body, or npos. The body's line
// endings may not match the file's, so each candidate is tried as-is, with
// CRs stripped, and with LFs expanded - whichever form is actually present is
// the one that matches, so the offset needs no remapping.
size_t FindConfiguredSignature(const std::string& body, const std::vector<std::string>& signatures)
{
	size_t best = std::string::npos;
	for (const std::string& raw : signatures) {
		std::string sig = raw;
		while (!sig.empty() && (sig.back() == '\r' || sig.back() == '\n' || sig.back() == ' ')) {
			sig.pop_back();
		}
		if (sig.size() < 5) continue; // too short to identify anything

		std::string lf;
		for (char c : sig) if (c != '\r') lf += c;
		std::string crlf;
		for (size_t i = 0; i < lf.size(); i++) {
			if (lf[i] == '\n') crlf += '\r';
			crlf += lf[i];
		}

		const std::string* forms[] = { &sig, &lf, &crlf };
		for (const std::string* form : forms) {
			size_t p = body.find(*form);
			if (p != std::string::npos && p < best) best = p;
		}
	}
	return best;
}

} // namespace

size_t FindPreservedTailStart(const std::string& utf8Text,
                               const std::vector<std::string>& signatures)
{
	static const char* kSeparators[] = {
		"-----Original Message-----",
		"-----Original message-----",
		"----- Original Message -----",
		"-----元のメッセージ-----",
		"-------- 転送されたメッセージ",
		"-------- Forwarded Message",
	};

	size_t cut = std::string::npos;
	for (const char* sep : kSeparators) {
		size_t p = utf8Text.find(sep);
		if (p != std::string::npos && p < cut) cut = p;
	}

	// A signature registered in Becky! is matched exactly, so it is found
	// wherever it sits - including above the quote, and in a new mail where
	// there is no quote at all.
	size_t sigAt = FindConfiguredSignature(utf8Text, signatures);
	if (sigAt < cut) cut = sigAt;

	size_t start = 0;
	size_t prevStart = std::string::npos; // previous line, if it was not blank
	while (start <= utf8Text.size() && start < cut) {
		size_t nl = utf8Text.find('\n', start);
		size_t end = (nl == std::string::npos) ? utf8Text.size() : nl;
		std::string line = Trim(utf8Text.substr(start, end - start));

		// "-- " on its own line is the RFC 3676 signature delimiter, and what
		// Becky!'s own signatures start with.
		bool isSignature = (line == "--");
		if (isSignature || (!line.empty() && line[0] == '>') || IsAttributionLine(line)) {
			cut = start;
			break;
		}
		if (IsRuleLine(line)) {
			// A rule is the underline of a quote header, not the header
			// itself: Becky! writes "Reply to <name> (<subject>)" on the line
			// above it. Start the quote there so the header survives.
			cut = (prevStart != std::string::npos) ? prevStart : start;
			break;
		}

		prevStart = line.empty() ? std::string::npos : start;
		if (nl == std::string::npos) break;
		start = nl + 1;
	}
	return cut;
}

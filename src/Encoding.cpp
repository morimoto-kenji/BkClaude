#include <windows.h>
#include "Encoding.h"

std::wstring Utf8ToWide(const std::string& utf8)
{
	if (utf8.empty()) return std::wstring();
	int len = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(), NULL, 0);
	std::wstring w(len, 0);
	MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(), &w[0], len);
	return w;
}

std::string WideToUtf8(const std::wstring& wide)
{
	if (wide.empty()) return std::string();
	int len = WideCharToMultiByte(CP_UTF8, 0, wide.data(), (int)wide.size(), NULL, 0, NULL, NULL);
	std::string s(len, 0);
	WideCharToMultiByte(CP_UTF8, 0, wide.data(), (int)wide.size(), &s[0], len, NULL, NULL);
	return s;
}

std::string AnsiToUtf8(const std::string& ansi, unsigned int codepage)
{
	if (ansi.empty()) return std::string();
	UINT cp = codepage ? codepage : CP_ACP;
	int wlen = MultiByteToWideChar(cp, 0, ansi.data(), (int)ansi.size(), NULL, 0);
	if (wlen <= 0) {
		// The claimed codepage couldn't decode this text; fall back to the
		// system codepage rather than returning nothing.
		cp = CP_ACP;
		wlen = MultiByteToWideChar(cp, 0, ansi.data(), (int)ansi.size(), NULL, 0);
		if (wlen <= 0) return std::string();
	}
	std::wstring w(wlen, 0);
	MultiByteToWideChar(cp, 0, ansi.data(), (int)ansi.size(), &w[0], wlen);
	return WideToUtf8(w);
}

std::string Utf8ToAnsi(const std::string& utf8, unsigned int codepage)
{
	std::wstring w = Utf8ToWide(utf8);
	if (w.empty()) return std::string();
	UINT cp = codepage ? codepage : CP_ACP;
	int alen = WideCharToMultiByte(cp, 0, w.data(), (int)w.size(), NULL, 0, NULL, NULL);
	std::string a(alen, 0);
	WideCharToMultiByte(cp, 0, w.data(), (int)w.size(), &a[0], alen, NULL, NULL);
	return a;
}

#pragma once
#include <string>

// Becky!'s own API is ANSI (system codepage, e.g. Shift_JIS on a Japanese
// Windows install); Claude API and our own dialogs work in UTF-8/UTF-16.
// These helpers sit at that boundary.

std::wstring Utf8ToWide(const std::string& utf8);
std::string WideToUtf8(const std::wstring& wide);

// codepage defaults to CP_ACP (the system codepage); pass the value from
// CBeckyAPI::GetCharSet() when converting a specific mail's text, since a
// mail's own charset does not always match the system codepage.
std::string AnsiToUtf8(const std::string& ansi, unsigned int codepage = 0 /*CP_ACP*/);
std::string Utf8ToAnsi(const std::string& utf8, unsigned int codepage = 0 /*CP_ACP*/);

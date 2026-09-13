#pragma once
#include <windows.h>
#include <string>

// Small helpers with no feature of their own, shared by the mail actions, the
// compose actions, the attachment check and the learning job. They live here
// rather than in one of those so that none of them has to include another.

// Upper bound on the body text sent in one request, in UTF-8 bytes.
const size_t kMaxBodyChars = 12000;

// Cuts a UTF-8 string to at most maxBytes without splitting a multi-byte
// character.
std::string TruncateUtf8(const std::string& s, size_t maxBytes);

std::string ToLowerAscii(std::string s);

// Strips leading and trailing blanks (space, tab, CR, LF).
std::string TrimBlank(const std::string& s);

// Loads the API key and the configured model, or tells the user the key is
// missing and returns false. Every feature starts with this.
bool RequireApiKey(std::string& outKey, std::string& outModel);

// The recipient addresses from a To header, lowercased, at most `maxCount`.
std::string ExtractAddresses(const std::string& header, size_t maxCount);

// Drops lines that look like quoted history ('>' prefix), leaving only what
// the user wrote themselves.
std::string StripQuotedLines(const std::string& utf8Text);

// One header field of the current mail / of the message being composed,
// MIME-decoded and converted to UTF-8, folded lines flattened to one.
std::string GetHeaderUtf8(const char* name);
std::string CompGetHeaderUtf8(HWND hWnd, const char* name);

// Which mailbox a compose window belongs to ("1234abcd.mb\"): from
// X-Becky-Ref for a reply, from the From header for a new mail, and from the
// main window's current folder as a last resort. Empty if none of those work.
std::string MailboxIdForCompose(HWND hWnd);

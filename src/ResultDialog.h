#pragma once
#include <windows.h>
#include <string>

// The dialogs that show a model's answer about an existing mail (要約 / 翻訳(日)),
// and the rich-text rendering they share with the compose-side confirmation
// dialog.

// Parameters for a result dialog that starts the Claude call itself once
// it is on screen, so "processing..." is visible before any network I/O
// begins. Stays alive for the whole dialog lifetime (see ShowResultDialog);
// stashed on the dialog via GWLP_USERDATA so the Reply/Forward buttons can
// reach ownerHwnd later.
struct ResultDlgRequest
{
	std::wstring title;
	std::string apiKey;
	std::string model;
	std::string systemPrompt;
	std::string userContent;
	int maxTokens;
	HWND ownerHwnd; // the mail list/view window the original mail belongs to
	bool sectioned = false; // use the three-pane dialog (summarize)

	// Cache (see Cache.h): if hasCachedResult, the dialog shows
	// cachedResultUtf8 immediately and never calls Claude. Otherwise, a
	// successful call is saved under cacheFeature/cacheMailId for next time.
	bool hasCachedResult = false;
	std::string cachedResultUtf8;
	std::string cacheFeature;
	std::string cacheVariant;
	std::string cacheMailId;
	int cacheMinutes = 0;
};

// Shows the dialog (modal), which then runs the request itself. `req` is
// only read while this call is on the stack, so a local variable is fine.
void ShowResultDialog(ResultDlgRequest req);

// Renders markdown-ish model output into a RICHEDIT50W control: bullets by
// depth, paragraph spacing, half-width folding of full-width ASCII, link
// detection. Used by the result panes and by the 校正 reason pane.
void SetRichTextWithIndents(HWND hEdit, const std::wstring& rawText);

// Gives a plain EDIT control the Ctrl+A (select all) shortcut. The Win32 EDIT
// control has never implemented it - only its right-click menu offers Select
// All - so every application that wants it adds it itself.
void EnableSelectAllShortcut(HWND hEdit);

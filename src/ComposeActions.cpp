#include <windows.h>
#include <commctrl.h>
#include <richedit.h>
#include <string>
#include <vector>
#include <memory>
#include <algorithm>
#include "ComposeActions.h"
#include "resource.h"
#include "BeckyApi.h"
#include "Settings.h"
#include "AsyncRunner.h"
#include "Encoding.h"
#include "MailSource.h"
#include "DiffView.h"
#include "ResultDialog.h"
#include "PluginUtil.h"

extern CBeckyAPI bka;
extern HINSTANCE g_hInstance;

namespace {

// A plain "working on it" window for the compose-window actions. Those wait
// on a worker thread rather than looping on the UI thread, so this one needs
// no timer and no forced repaint.
HWND s_busyDlg = NULL;

INT_PTR CALLBACK BusyDlgProc(HWND, UINT, WPARAM, LPARAM)
{
	return FALSE;
}

void OpenBusy(HWND hOwner, const wchar_t* text)
{
	if (s_busyDlg) DestroyWindow(s_busyDlg);
	s_busyDlg = CreateDialogW(g_hInstance, MAKEINTRESOURCEW(IDD_BUSY),
		hOwner, BusyDlgProc);
	if (!s_busyDlg) return;
	SetDlgItemTextW(s_busyDlg, IDC_BUSY_TEXT, text);
	ShowWindow(s_busyDlg, SW_SHOWNORMAL);
}

void CloseBusy()
{
	if (!s_busyDlg) return;
	DestroyWindow(s_busyDlg);
	s_busyDlg = NULL;
}

} // namespace

namespace {

// The three compose-window actions. All of them work on the same input - the
// part of the body above the quote, i.e. what the user actually wrote - and
// differ only in what they ask Claude to do with it.
enum ComposeTask { kTaskDraft, kTaskProofread, kTaskTranslateEn };

std::string ToCrlfUtf8(const std::string& s)
{
	std::string out;
	out.reserve(s.size() + s.size() / 16);
	for (size_t i = 0; i < s.size(); i++) {
		if (s[i] == '\n' && (i == 0 || s[i - 1] != '\r')) out += '\r';
		out += s[i];
	}
	return out;
}

// Observed: a model prefixed its answer with "Rewritten body:". The generic
// "no commentary" rule did not cover it - a label apparently does not read as
// commentary - so the failure has to be named outright, exactly as with the
// 「前提:」「最新:」 labels in the summary.
const char* kNoPreambleRule =
	"\n\nThe first character of your output is the first character of the mail "
	"body. Never open with a label, heading or preamble of any kind - not "
	"'Rewritten body:', not '校正後:', not a restatement of the task, not a "
	"blank line. There is nothing to introduce: your output is inserted "
	"directly into the message as it stands.";

const char* kNoSignatureRule =
	"\n\nNever write a signature block - no name/organisation/address/phone/"
	"email/URL sign-off. The mail client appends the user's real signature "
	"itself, so any signature you write would be a duplicate. This ban covers "
	"signature blocks only: a closing greeting such as 「よろしくお願いします」 "
	"is body text, not a signature. Whether to use one is a matter of style "
	"and situation - leave it out where the user would (a one-line "
	"acknowledgement, for instance).";

// Preview shown before an action overwrites what the user wrote. `before`
// empty means no diff is meaningful (a translation shares no text with its
// source), and the proposed text is simply shown as-is.
struct ConfirmRequest
{
	std::wstring hint;
	std::wstring before;
	std::wstring after;
	std::wstring reason; // empty hides that pane and grows the diff to fill
};

const char* kSecProofBody = "【校正後の本文】";
const char* kSecProofWhy = "【修正の理由】";

// Splits the proofread answer into the corrected body and the explanation.
// Returns false when the model did not follow the format, in which case the
// whole answer is treated as the body - the same fallback the summary uses.
bool SplitProofread(const std::string& utf8, std::string& body, std::string& why)
{
	size_t b = utf8.find(kSecProofBody);
	size_t w = utf8.find(kSecProofWhy);
	if (b == std::string::npos || w == std::string::npos || b > w) return false;
	body = TrimBlank(utf8.substr(b + strlen(kSecProofBody), w - b - strlen(kSecProofBody)));
	why = TrimBlank(utf8.substr(w + strlen(kSecProofWhy)));
	return !body.empty();
}

INT_PTR CALLBACK ConfirmDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
	switch (msg) {
	case WM_INITDIALOG: {
		ConfirmRequest* req = (ConfirmRequest*)lParam;
		SetDlgItemTextW(hDlg, IDC_CONFIRM_HINT, req->hint.c_str());

		HWND hText = GetDlgItem(hDlg, IDC_CONFIRM_TEXT);
		if (req->before.empty()) {
			SetRichTextWithIndents(hText, req->after);
		} else {
			SetRichTextWithDiff(hText, req->before, req->after);
		}

		HWND hWhy = GetDlgItem(hDlg, IDC_CONFIRM_REASON);
		HWND hWhyLabel = GetDlgItem(hDlg, IDC_CONFIRM_REASON_LABEL);
		if (req->reason.empty()) {
			// No explanation to show (a translation, or the model ignored the
			// format) - give the space back to the text rather than leaving
			// an empty box.
			ShowWindow(hWhy, SW_HIDE);
			ShowWindow(hWhyLabel, SW_HIDE);
			RECT rcText, rcWhy, rcDlg;
			GetWindowRect(hText, &rcText);
			GetWindowRect(hWhy, &rcWhy);
			MapWindowPoints(NULL, hDlg, (LPPOINT)&rcText, 2);
			MapWindowPoints(NULL, hDlg, (LPPOINT)&rcWhy, 2);
			SetWindowPos(hText, NULL, 0, 0, rcText.right - rcText.left,
				rcWhy.bottom - rcText.top, SWP_NOMOVE | SWP_NOZORDER);
			(void)rcDlg;
		} else {
			SetRichTextWithIndents(hWhy, req->reason);
		}

		// Focus the button, not the text: a focused read-only edit selects
		// its whole content, which hides the very colouring being shown.
		SetFocus(GetDlgItem(hDlg, IDOK));
		return FALSE;
	}
	case WM_COMMAND:
		if (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL) {
			EndDialog(hDlg, LOWORD(wParam));
			return TRUE;
		}
		break;
	}
	return FALSE;
}

bool ConfirmProposedText(HWND hOwner, const std::wstring& hint,
                          const std::wstring& before, const std::wstring& after,
                          const std::wstring& reason)
{
	static HMODULE s_richEdit = NULL;
	if (!s_richEdit) s_richEdit = LoadLibraryW(L"Msftedit.dll");

	ConfirmRequest req;
	req.hint = hint;
	req.before = before;
	req.after = after;
	req.reason = reason;
	return DialogBoxParamW(g_hInstance, MAKEINTRESOURCEW(IDD_CONFIRM),
		hOwner, ConfirmDlgProc, (LPARAM)&req) == IDOK;
}

void RunComposeTask(HWND hWnd, ComposeTask task)
{
	std::string apiKey, model;
	if (!RequireApiKey(apiKey, model)) return;

	char mimeType[64] = {};
	LPSTR lpBody = bka.CompGetText(hWnd, mimeType, sizeof(mimeType));
	std::string rawBody = (lpBody && *lpBody) ? lpBody : "";

	// HTML mail is deliberately out of scope. Refuse explicitly rather than
	// relying on the observation that BKC_MENU_COMPEDIT does not seem to fire
	// in the HTML editor: if it ever did, we would hand Claude raw markup and
	// then replace the formatted body with plain text.
	if (ToLowerAscii(mimeType).find("html") != std::string::npos) {
		MessageBoxW(hWnd,
			L"HTML形式のメールでは使用できません。\r\n\r\n"
			L"この機能はテキスト形式のメールでのみ動作します。",
			L"Becky! Claude Plugin", MB_OK | MB_ICONINFORMATION);
		return;
	}

	char charsetName[64] = {};
	int codepage = bka.CompGetCharSet(hWnd, charsetName, sizeof(charsetName));
	unsigned int cp = codepage > 0 ? (unsigned int)codepage : 0;
	std::string utf8Body = AnsiToUtf8(rawBody, cp);

	// Split off everything that must survive verbatim - the quote, the
	// signature, or both - and keep it here. It never enters the model's
	// output path, so it can neither come back altered nor be replaced away.
	size_t tailAt = FindPreservedTailStart(utf8Body, LoadAllSignatures());
	std::string userPart = (tailAt == std::string::npos) ? utf8Body : utf8Body.substr(0, tailAt);
	std::string quotePart = (tailAt == std::string::npos) ? std::string() : utf8Body.substr(tailAt);

	bool nothingWritten = userPart.find_first_not_of(" \t\r\n") == std::string::npos;
	if (task != kTaskDraft && nothingWritten) {
		MessageBoxW(hWnd,
			L"対象となる本文がありません。\r\n\r\n"
			L"引用部より上に、ご自身が書かれた文章を入力してから実行してください。",
			L"Becky! Claude Plugin", MB_OK | MB_ICONWARNING);
		return;
	}

	std::string systemPrompt;
	bool useStyleCard = false;

	if (task == kTaskDraft) {
		systemPrompt =
			"You draft Japanese business email replies. The input gives you the "
			"user's rough notes for their reply - which may be only keywords, or "
			"empty - plus the original message being replied to, for context.\n\n"
			"Turn the notes into a complete, sendable Japanese email body. Keep "
			"every point the notes make. Do NOT invent commitments, dates, "
			"numbers or facts that appear in neither the notes nor the original "
			"message. If there are no notes at all, write a short, safe reply to "
			"the original message for the user to edit.\n\n"
			"Output ONLY the reply body - no markdown, no commentary, and never "
			"reproduce the quoted original.";
		systemPrompt += kNoSignatureRule;
		systemPrompt += kNoPreambleRule;
		useStyleCard = true;
	} else if (task == kTaskProofread) {
		systemPrompt =
			"You proofread Japanese business email that the user has already "
			"written and intends to send.\n\n"
			"Fix what is wrong: typos, mistaken kanji conversions, grammar, "
			"inconsistent or incorrect keigo, unnatural phrasing. This is a "
			"correction pass, not a rewrite - keep the user's own voice, "
			"meaning, structure, paragraphs and line breaks. Do not add or "
			"remove content, and do not reorganise a message that reads fine.\n\n"
			"Output exactly these two sections, with these headings verbatim "
			"and nothing before or after them:\n"
			"\n"
			"【校正後の本文】\n"
			"The corrected body, and nothing else.\n"
			"\n"
			"【修正の理由】\n"
			"One bullet per change you made, each quoting the original wording "
			"and saying why it was changed. One line each. Never list a change "
			"you did not make. If you changed nothing, say 修正はありません.\n"
			"\n"
			"This section records what you did; it is NOT a filter on what to "
			"do. Make every correction the text needs first, then account for "
			"them. Do not skip a correction because it is awkward to justify - "
			"where an improvement is hard to name, name it plainly (語順を整えた, "
			"表現を自然にした) and keep the correction. A short list of changes "
			"is only right if the text was already nearly clean.";
		systemPrompt += kNoSignatureRule;
		useStyleCard = true;
	} else {
		systemPrompt =
			"The user has written an email body and wants to send it in English.\n\n"
			"Produce the English email body. If the text is Japanese, render it "
			"as natural business English rather than a literal translation; if it "
			"is already English, correct and improve it - grammar, phrasing, "
			"typos. Match the formality to the recipient and to the message being "
			"replied to. Preserve the paragraph structure.\n\n"
			"Output ONLY the English body - no markdown, no commentary, no "
			"translator's notes.";
		systemPrompt += kNoSignatureRule;
		systemPrompt += kNoPreambleRule;
		// No style card here: it describes keigo and Japanese first-person
		// usage, which have no counterpart in English.
	}

	if (useStyleCard) {
		std::string styleCard;
		LoadStyleProfile(MailboxIdForCompose(hWnd), styleCard);
		if (!styleCard.empty()) {
			systemPrompt +=
				"\n\nThe style guide below was learned from this account's own "
				"sent mail. It describes how this person writes, including how "
				"their register varies by recipient - pick the one that fits the "
				"recipient in the headers.";
			systemPrompt += (task == kTaskProofread)
				? " Use it to judge what \"correct for this writer\" means: keep "
				  "their voice rather than normalising towards generic business "
				  "Japanese. It is a reference, not a target to rewrite into."
				: " Write as this person writes.";
			systemPrompt += "\n\n--- User's writing style ---\n" + styleCard + "\n--- end ---";
			// The guide keys its rules on the recipient's address, so a person
			// who appears in two unrelated threads gets one rule built mostly
			// from whichever thread supplied more samples - and it then leaks
			// into the other. The thread in hand is the more specific evidence
			// and has to win.
			systemPrompt +=
				"\n\nWhere these disagree, prefer the more specific evidence:\n"
				"1. Anything the user themselves wrote earlier in the quoted "
				"thread. That is how they actually write to this person about "
				"this matter, and it outranks any general rule.\n"
				"2. The style guide's rule for this recipient.\n"
				"3. The style guide's general description.\n"
				"The other party's words in the quote are deliberately NOT on "
				"that list. They show how formal the relationship is and what "
				"register to answer in; they are never a model to copy. Write "
				"in the user's voice, never the correspondent's.";
		}
	}

	std::string toLine = CompGetHeaderUtf8(hWnd, "To");
	std::string ccLine = CompGetHeaderUtf8(hWnd, "Cc");
	std::string subjectLine = CompGetHeaderUtf8(hWnd, "Subject");

	// The style guide describes openings and closings separately for replies
	// and new mail, so it needs to be told which this is. X-Becky-Ref is the
	// authoritative signal: present on a reply or forward, absent on a new
	// message - unlike the quote, which the user may have deleted.
	char composeRef[64] = {};
	bka.CompGetSpecifiedHeader(hWnd, "X-Becky-Ref", composeRef, sizeof(composeRef));
	bool isReplyCompose = composeRef[0] != '\0';

	std::string userPrompt = isReplyCompose
		? "--- This is a reply to an existing message ---\n"
		: "--- This is a new message; it opens the correspondence ---\n";
	if (!toLine.empty() || !ccLine.empty() || !subjectLine.empty()) {
		userPrompt += "--- This mail will be sent to ---\n";
		if (!toLine.empty())      userPrompt += "To: " + toLine + "\n";
		if (!ccLine.empty())      userPrompt += "Cc: " + ccLine + "\n";
		if (!subjectLine.empty()) userPrompt += "Subject: " + subjectLine + "\n";
	}
	if (!quotePart.empty()) {
		userPrompt += "\n--- The message being replied to (context only, never reproduce it) ---\n";
		userPrompt += TruncateUtf8(quotePart, kMaxBodyChars);
	}
	userPrompt += (task == kTaskDraft)
		? "\n--- The user's notes for the reply ---\n"
		: "\n--- What the user wrote ---\n";
	userPrompt += nothingWritten ? "(none)" : TruncateUtf8(userPart, kMaxBodyChars);

	OpenBusy(hWnd, (task == kTaskDraft)      ? L"下書きを作成しています..." :
	               (task == kTaskProofread)  ? L"本文を校正しています..."
	                                         : L"英文に変換しています...");

	// Proofreading returns roughly what it was given, so the output cap has
	// to exceed the input cap (kMaxBodyChars, ~4000 Japanese characters).
	// 2048 could not have held a long mail, and would now fail the whole
	// request rather than truncate it.
	AsyncRunner_CallClaude(apiKey, model, systemPrompt, userPrompt, 8192,
		[hWnd, cp, quotePart, task, userPart](bool success, const std::string& text, const std::string& error) {
			CloseBusy();
			if (!IsWindow(hWnd)) return; // compose window closed while waiting
			if (!success) {
				MessageBoxW(hWnd, (L"エラー: " + Utf8ToWide(error)).c_str(),
					L"Becky! Claude Plugin", MB_OK | MB_ICONERROR);
				return;
			}

			// 校正 and 翻訳(英) replace prose the user wrote themselves - the
			// only destructive actions here - so nothing is applied until it
			// has been seen and accepted. 下書き is exempt: it consumes the
			// rough notes it was given, which is the point of it.
			std::string applied = text;

			if (task != kTaskDraft) {
				bool isProofread = (task == kTaskProofread);
				std::wstring reason;
				if (isProofread) {
					std::string body, why;
					if (SplitProofread(text, body, why)) {
						applied = body;
						reason = Utf8ToWide(why);
					}
				}
				std::wstring hint = isProofread
					? L"赤の取り消し線は削除、青は追加です。\r\n"
					  L"折り返しは表示用に調整しています（本文には影響しません）。"
					: L"英訳案です。適用すると本文が置き換わります。";
				// A translation shares no text with its source, so a diff
				// against the Japanese would be noise; show it plain.
				std::wstring before = isProofread ? Utf8ToWide(userPart) : std::wstring();
				if (!ConfirmProposedText(hWnd, hint, before, Utf8ToWide(applied), reason)) {
					return;
				}
			}

			std::string body = ToCrlfUtf8(applied);
			if (!quotePart.empty()) {
				body += "\r\n\r\n";
				body += quotePart; // verbatim, exactly as the editor had it
			}
			bka.CompSetText(hWnd, 0, Utf8ToAnsi(body, cp).c_str());
		});
}

} // namespace

void CALLBACK OnCmdDraftReply(HWND hWnd, LPARAM /*lParam*/)
{
	RunComposeTask(hWnd, kTaskDraft);
}

void CALLBACK OnCmdProofread(HWND hWnd, LPARAM /*lParam*/)
{
	RunComposeTask(hWnd, kTaskProofread);
}

void CALLBACK OnCmdTranslateEnglish(HWND hWnd, LPARAM /*lParam*/)
{
	RunComposeTask(hWnd, kTaskTranslateEn);
}

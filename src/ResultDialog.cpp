#include <windows.h>
#include <commctrl.h>
#include <richedit.h>
#include <shellapi.h>
#include <string>
#include <vector>
#include <algorithm>
#include "ResultDialog.h"
#include "resource.h"
#include "BeckyApi.h"
#include "AsyncRunner.h"
#include "Encoding.h"
#include "Cache.h"
#include "PluginUtil.h"

extern CBeckyAPI bka;
extern HINSTANCE g_hInstance;

namespace {

// A plain Win32 edit control only breaks lines on CRLF; Claude's output
// uses bare LF, which would otherwise render as one run-together paragraph.
std::wstring ToCrlf(const std::wstring& text)
{
	std::wstring out;
	out.reserve(text.size());
	for (size_t i = 0; i < text.size(); i++) {
		if (text[i] == L'\n' && (i == 0 || text[i - 1] != L'\r')) {
			out += L'\r';
		}
		out += text[i];
	}
	return out;
}

// The summarize prompt emits these three headings verbatim; the sectioned
// result dialog splits the answer on them and drops the markers (each pane
// already carries the heading as its group box label).
const char* kSecPeople = "【メール送受信者】";
const char* kSecSummary = "【メールの要約】";
const char* kSecAction = "【メールへの推奨対応】";

struct ResultSections
{
	bool ok = false;
	std::string people, summary, action;
};

ResultSections SplitSections(const std::string& utf8)
{
	ResultSections s;
	size_t p = utf8.find(kSecPeople);
	size_t m = utf8.find(kSecSummary);
	size_t a = utf8.find(kSecAction);
	if (p == std::string::npos || m == std::string::npos || a == std::string::npos ||
	    !(p < m && m < a)) {
		return s; // model didn't follow the format; caller falls back
	}
	size_t pEnd = p + strlen(kSecPeople);
	size_t mEnd = m + strlen(kSecSummary);
	size_t aEnd = a + strlen(kSecAction);
	s.people = TrimBlank(utf8.substr(pEnd, m - pEnd));
	s.summary = TrimBlank(utf8.substr(mEnd, a - mEnd));
	s.action = TrimBlank(utf8.substr(aEnd));
	s.ok = true;
	return s;
}

// Puts text into a RichEdit with real paragraph indents: the model's leading
// spaces are stripped and turned into a per-level left indent plus a hanging
// indent, so a wrapped line lines up with its own text instead of falling
// back to the left margin.
// Japanese mail freely mixes full-width and half-width letters and digits, and
// the summary inherits the mix. Only letters and digits are folded: full-width
// punctuation like ！？（） is a deliberate part of Japanese typography and
// converting it would look wrong.
std::wstring NormalizeAsciiWidth(const std::wstring& s)
{
	std::wstring out = s;
	for (wchar_t& c : out) {
		if ((c >= 0xFF21 && c <= 0xFF3A) ||  // Ａ-Ｚ
		    (c >= 0xFF41 && c <= 0xFF5A) ||  // ａ-ｚ
		    (c >= 0xFF10 && c <= 0xFF19)) {  // ０-９
			c -= 0xFEE0;
		}
	}
	return out;
}

} // namespace

void SetRichTextWithIndents(HWND hEdit, const std::wstring& rawText)
{
	std::wstring text = NormalizeAsciiWidth(rawText);

	// Must be on before the text is set, or the new text is not scanned.
	SendMessageW(hEdit, EM_AUTOURLDETECT, TRUE, 0);
	SendMessageW(hEdit, EM_SETEVENTMASK, 0,
		SendMessageW(hEdit, EM_GETEVENTMASK, 0, 0) | ENM_LINK);

	std::vector<std::wstring> lines;
	std::vector<int> depths;
	size_t start = 0;
	while (start <= text.size()) {
		size_t nl = text.find(L'\n', start);
		size_t end = (nl == std::wstring::npos) ? text.size() : nl;
		std::wstring line = text.substr(start, end - start);
		if (!line.empty() && line.back() == L'\r') line.pop_back();

		// Blank lines are dropped: the spacing between items is applied as
		// paragraph spacing below, and keeping both doubles the gap.
		size_t firstText = line.find_first_not_of(L' ');
		if (firstText == std::wstring::npos) {
			if (nl == std::wstring::npos) break;
			start = nl + 1;
			continue;
		}
		int depth = (int)(firstText / 2);
		line.erase(0, firstText);

		// The model writes markdown's "- ". Swap in a real mark, one per
		// level, so the hierarchy reads at a glance. ●/○/・ is the ordinary
		// Japanese ordering and all three are safe in a Japanese UI font.
		if (line.rfind(L"- ", 0) == 0) {
			const wchar_t* mark = (depth == 0) ? L"● " : (depth == 1) ? L"○ " : L"・";
			line = std::wstring(mark) + line.substr(2);
		}

		lines.push_back(line);
		depths.push_back(depth);
		if (nl == std::wstring::npos) break;
		start = nl + 1;
	}

	// RichEdit stores a paragraph break as a single CR, so joining with CR
	// keeps our character offsets below in step with the control's.
	std::wstring joined;
	for (size_t i = 0; i < lines.size(); i++) {
		if (i) joined += L'\r';
		joined += lines[i];
	}
	SetWindowTextW(hEdit, joined.c_str());

	// RichEdit's default face is small and dense for Japanese. A UI font at
	// 10pt is the single biggest readability win here.
	CHARFORMAT2W cf{};
	cf.cbSize = sizeof(cf);
	cf.dwMask = CFM_FACE | CFM_SIZE;
	wcscpy_s(cf.szFaceName, L"Yu Gothic UI");
	cf.yHeight = 200; // twips; 20 per point
	SendMessageW(hEdit, EM_SETSEL, 0, -1);
	SendMessageW(hEdit, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&cf);

	long pos = 0;
	for (size_t i = 0; i < lines.size(); i++) {
		SendMessageW(hEdit, EM_SETSEL, pos, pos + (LPARAM)lines[i].size());
		PARAFORMAT2 pf{};
		pf.cbSize = sizeof(pf);
		pf.dwMask = PFM_STARTINDENT | PFM_OFFSET | PFM_SPACEBEFORE;
		pf.dxStartIndent = 60 + depths[i] * 240; // twips (1440 = 1 inch)
		pf.dxOffset = 180;                        // hanging indent for wrapped lines
		// Air before a top-level bullet only: that is what groups its children
		// with it. Sub-items stay tight against their parent, and plain
		// paragraphs (the people list, a verdict line) stay tight too - they
		// are meant to be taken in at a glance, not spaced out.
		bool topBullet = (depths[i] == 0) && lines[i].rfind(L"● ", 0) == 0;
		pf.dySpaceBefore = (i == 0 || !topBullet) ? 0 : 90;
		SendMessageW(hEdit, EM_SETPARAFORMAT, 0, (LPARAM)&pf);
		pos += (long)lines[i].size() + 1;
	}
	SendMessageW(hEdit, EM_SETSEL, 0, 0);
}

namespace {

LRESULT CALLBACK SelectAllEditProc(HWND hEdit, UINT msg, WPARAM wParam, LPARAM lParam,
                                    UINT_PTR, DWORD_PTR)
{
	if (msg == WM_KEYDOWN && wParam == 'A' && (GetKeyState(VK_CONTROL) & 0x8000)) {
		SendMessageW(hEdit, EM_SETSEL, 0, -1);
		return 0;
	}
	// Ctrl+A also arrives as WM_CHAR 0x01, which the edit control rejects
	// with a beep. Swallow it.
	if (msg == WM_CHAR && wParam == 1) {
		return 0;
	}
	if (msg == WM_NCDESTROY) {
		RemoveWindowSubclass(hEdit, SelectAllEditProc, 0);
	}
	return DefSubclassProc(hEdit, msg, wParam, lParam);
}

// A "link" here is model output derived from mail content, i.e. untrusted
// input. Handing an arbitrary string to ShellExecute would let a crafted mail
// launch a local file or a registered protocol handler, so only schemes that
// cannot reach the local machine are followed.
void OpenLinkSafely(HWND hDlg, const std::wstring& url)
{
	auto startsWith = [&url](const wchar_t* prefix) {
		size_t n = wcslen(prefix);
		return url.size() >= n && _wcsnicmp(url.c_str(), prefix, n) == 0;
	};
	if (!startsWith(L"http://") && !startsWith(L"https://") && !startsWith(L"mailto:")) {
		return;
	}
	ShellExecuteW(hDlg, L"open", url.c_str(), NULL, NULL, SW_SHOWNORMAL);
}

struct ActionChoice { UINT id; const wchar_t* label; const char* command; };

// Pops up a small menu above/right of `button` and returns the chosen
// choice's bka.Command() string, or NULL if the user dismissed it.
const char* ShowActionPopup(HWND hDlg, HWND button, const ActionChoice* choices, int count)
{
	HMENU hPopup = CreatePopupMenu();
	for (int i = 0; i < count; i++) {
		AppendMenuW(hPopup, MF_STRING, choices[i].id, choices[i].label);
	}
	RECT rc;
	GetWindowRect(button, &rc);
	int chosenId = TrackPopupMenu(hPopup, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN | TPM_NONOTIFY,
		rc.left, rc.bottom, 0, hDlg, NULL);
	DestroyMenu(hPopup);
	for (int i = 0; i < count; i++) {
		if (choices[i].id == (UINT)chosenId) return choices[i].command;
	}
	return NULL;
}

const ActionChoice kReplyChoices[] = {
	{ 2001, L"返信", "Reply" },
	{ 2002, L"全員に返信", "ReplyAll" },
	{ 2003, L"任意のアドレスに返信", "ReplyAddress" },
};
const ActionChoice kForwardChoices[] = {
	{ 2010, L"自分を差出人として転送", "Forward" },
	{ 2011, L"手を加えずに転送", "ForwardRedirect" },
	{ 2012, L"元のメールを添付して転送", "ForwardRFC822" },
};

void RunAction(HWND hDlg, const char* cmd)
{
	ResultDlgRequest* req = (ResultDlgRequest*)GetWindowLongPtrW(hDlg, GWLP_USERDATA);
	HWND owner = req ? req->ownerHwnd : NULL;
	EndDialog(hDlg, IDOK);
	bka.Command(owner, cmd);
}

// Main body of the split button (WM_COMMAND/BN_CLICKED): run the default
// action directly, same as Becky!'s own reply/forward toolbar buttons.
void RunDefaultAction(HWND hDlg, UINT id)
{
	RunAction(hDlg, id == IDC_REPLY_BTN ? "Reply" : "Forward");
}

// Dropdown arrow (WM_NOTIFY/BCN_DROPDOWN): offer the other variants.
void ShowActionMenu(HWND hDlg, UINT id)
{
	bool isReply = (id == IDC_REPLY_BTN);
	const ActionChoice* choices = isReply ? kReplyChoices : kForwardChoices;
	const char* cmd = ShowActionPopup(hDlg, GetDlgItem(hDlg, id), choices, 3);
	if (cmd) {
		RunAction(hDlg, cmd);
	}
}

// Puts a finished (or failed) answer on screen. The sectioned dialog splits
// it into its three panes; the plain one drops it in a single box.
void DisplayResult(HWND hDlg, const std::string& utf8, bool isError)
{
	HWND hSummary = GetDlgItem(hDlg, IDC_RESULT_SUMMARY);
	if (!hSummary) { // plain single-pane dialog
		SetDlgItemTextW(hDlg, IDC_RESULT_TEXT, ToCrlf(Utf8ToWide(utf8)).c_str());
		return;
	}

	ResultSections sec = SplitSections(utf8);
	if (isError || !sec.ok) {
		// Errors, and answers that didn't follow the format, go in the
		// summary pane whole rather than being silently dropped - with a note
		// saying which, since on screen the two look identical.
		std::wstring note;
		if (!isError) {
			note = L"※ 応答が想定の3部構成になっていないため、そのまま表示しています。\r\n\r\n";
		}
		SetRichTextWithIndents(GetDlgItem(hDlg, IDC_RESULT_PEOPLE), L"");
		SetRichTextWithIndents(hSummary, note + Utf8ToWide(utf8));
		SetRichTextWithIndents(GetDlgItem(hDlg, IDC_RESULT_ACTION), L"");
		return;
	}
	SetRichTextWithIndents(GetDlgItem(hDlg, IDC_RESULT_PEOPLE), Utf8ToWide(sec.people));
	SetRichTextWithIndents(hSummary, Utf8ToWide(sec.summary));
	SetRichTextWithIndents(GetDlgItem(hDlg, IDC_RESULT_ACTION), Utf8ToWide(sec.action));
}

void SetWaitingText(HWND hDlg)
{
	HWND hSummary = GetDlgItem(hDlg, IDC_RESULT_SUMMARY);
	if (hSummary) {
		SetWindowTextW(hSummary, L"処理中...");
	} else {
		SetDlgItemTextW(hDlg, IDC_RESULT_TEXT, L"処理中...");
	}
}

INT_PTR CALLBACK ResultDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
	switch (msg) {
	case WM_INITDIALOG: {
		ResultDlgRequest* req = (ResultDlgRequest*)lParam;
		SetWindowLongPtrW(hDlg, GWLP_USERDATA, (LONG_PTR)req);
		SetWindowTextW(hDlg, req->title.c_str());
		// Only the single-pane dialog needs this; the RichEdit panes of the
		// sectioned one handle Ctrl+A themselves.
		EnableSelectAllShortcut(GetDlgItem(hDlg, IDC_RESULT_TEXT));
		// Focus OK, not the first pane: the dialog manager selects the whole
		// contents of any edit control it gives focus to, so the first thing
		// the user saw was the address list highlighted end to end. Same fix
		// as the 校正 confirmation dialog.
		SetFocus(GetDlgItem(hDlg, IDOK));
		if (req->hasCachedResult) {
			DisplayResult(hDlg, req->cachedResultUtf8, false);
			return FALSE;
		}
		SetWaitingText(hDlg);
		std::string feature = req->cacheFeature;
		std::string variant = req->cacheVariant;
		std::string mailId = req->cacheMailId;
		int cacheMinutes = req->cacheMinutes;
		AsyncRunner_CallClaude(req->apiKey, req->model, req->systemPrompt, req->userContent, req->maxTokens,
			[hDlg, feature, variant, mailId, cacheMinutes](bool success, const std::string& text, const std::string& error) {
				if (success) {
					SaveCachedResult(feature, variant, mailId, text, cacheMinutes);
				}
				if (!IsWindow(hDlg)) return; // dialog closed while waiting
				DisplayResult(hDlg, success ? text : ("エラー: " + error), !success);
			});
		return FALSE;
	}
	case WM_COMMAND: {
		UINT id = LOWORD(wParam);
		if (id == IDOK || id == IDCANCEL) {
			EndDialog(hDlg, IDOK);
			return TRUE;
		}
		if ((id == IDC_REPLY_BTN || id == IDC_FORWARD_BTN) && HIWORD(wParam) == BN_CLICKED) {
			RunDefaultAction(hDlg, id);
			return TRUE;
		}
		break;
	}
	case WM_NOTIFY: {
		LPNMHDR nmhdr = (LPNMHDR)lParam;
		if (nmhdr->code == EN_LINK) {
			ENLINK* el = (ENLINK*)lParam;
			if (el->msg == WM_LBUTTONUP) {
				LONG len = el->chrg.cpMax - el->chrg.cpMin;
				if (len > 0 && len < 2048) {
					std::wstring url((size_t)len + 1, L'\0');
					TEXTRANGEW tr{};
					tr.chrg = el->chrg;
					tr.lpstrText = &url[0];
					SendMessageW(nmhdr->hwndFrom, EM_GETTEXTRANGE, 0, (LPARAM)&tr);
					url.resize(wcslen(url.c_str()));
					OpenLinkSafely(hDlg, url);
				}
				return TRUE;
			}
			break;
		}
		if (nmhdr->code == BCN_DROPDOWN &&
		    (nmhdr->idFrom == IDC_REPLY_BTN || nmhdr->idFrom == IDC_FORWARD_BTN)) {
			ShowActionMenu(hDlg, (UINT)nmhdr->idFrom);
			return TRUE;
		}
		break;
	}
	}
	return FALSE;
}

} // namespace

// Shows the result dialog immediately (with a "processing..." placeholder,
// or the cached text if req.hasCachedResult) and only makes the Claude call
// once it is on screen; see ResultDlgProc. req is only read synchronously
// while this call is on the stack, so a local variable is fine.
void ShowResultDialog(ResultDlgRequest req)
{
	// RICHEDIT50W lives in Msftedit.dll; without it the sectioned template
	// has an unknown window class and the dialog fails to create.
	static HMODULE s_richEdit = NULL;
	if (!s_richEdit) s_richEdit = LoadLibraryW(L"Msftedit.dll");

	int templateId = req.sectioned ? IDD_RESULT_SECTIONS : IDD_RESULT;
	DialogBoxParamW(g_hInstance, MAKEINTRESOURCEW(templateId), NULL, ResultDlgProc, (LPARAM)&req);
}

void EnableSelectAllShortcut(HWND hEdit)
{
	if (hEdit) SetWindowSubclass(hEdit, SelectAllEditProc, 0, 0);
}

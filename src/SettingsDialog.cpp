#include <windows.h>
#include <commctrl.h>
#include <string>
#include <vector>
#include <memory>
#include <cstdio>
#include "SettingsDialog.h"
#include "resource.h"
#include "Settings.h"
#include "AsyncRunner.h"
#include "AiClient.h"
#include "Cache.h"
#include "ResultDialog.h"
#include "Encoding.h"
#include "BeckyApi.h"

extern CBeckyAPI bka;

extern HINSTANCE g_hInstance;

namespace {

struct ModelChoice
{
	const char* id;
	const char* label;
};

// Description text is Anthropic's own wording from the Models overview page.
const ModelChoice kModels[] = {
	{ "claude-haiku-4-5", "Claude Haiku 4.5 - The fastest model with near-frontier intelligence" },
	{ "claude-sonnet-5",  "Claude Sonnet 5 - The best combination of speed and intelligence" },
	{ "claude-opus-5",    "Claude Opus 5 - For complex agentic coding and enterprise work" },
};
const int kModelCount = sizeof(kModels) / sizeof(kModels[0]);

// Shown (masked) when a key is already saved; its content is irrelevant -
// s_keyDirty is the actual "user changed the key" signal.
const char* kKeyPlaceholder = "xxxxxxxxxxxx";
bool s_keyDirty = false;
bool s_keyPlaceholderShown = false;
int s_lastModelSel = -1; // to clear the log only on an actual model change

// A stored key can't be edited in place, only replaced. So on the first
// real input into the key field, wipe the placeholder and let the input
// land in an empty field.
LRESULT CALLBACK ApiKeyEditProc(HWND hEdit, UINT msg, WPARAM wParam, LPARAM lParam,
                                 UINT_PTR, DWORD_PTR)
{
	bool editing = (msg == WM_CHAR) || (msg == WM_PASTE) || (msg == WM_CUT) ||
	               (msg == WM_CLEAR) ||
	               (msg == WM_KEYDOWN && (wParam == VK_DELETE || wParam == VK_BACK));
	if (editing && s_keyPlaceholderShown) {
		s_keyPlaceholderShown = false;
		SetWindowTextW(hEdit, L"");
		// Now entering a brand-new key - show it as plain text while typing.
		SendMessageW(hEdit, EM_SETPASSWORDCHAR, 0, 0);
		InvalidateRect(hEdit, NULL, TRUE);
	}
	if (msg == WM_NCDESTROY) {
		RemoveWindowSubclass(hEdit, ApiKeyEditProc, 0);
	}
	return DefSubclassProc(hEdit, msg, wParam, lParam);
}

// Registered Sent folders, in the same order as the list box rows.
std::vector<SentFolderInfo> s_sentFolders;

// "claude-haiku-4-5" -> "Haiku 4.5". The full id is what gets stored and what
// the API needs, but in a list row it is mostly boilerplate; the part that
// actually distinguishes one entry from another is the last few characters.
// An id we don't know (a dated snapshot, say) is shown verbatim.
std::string ShortModelName(const std::string& id)
{
	for (int i = 0; i < kModelCount; i++) {
		if (id != kModels[i].id) continue;
		std::string label = kModels[i].label;
		size_t dash = label.find(" - ");
		if (dash != std::string::npos) label.erase(dash);
		const std::string kPrefix = "Claude ";
		if (label.compare(0, kPrefix.size(), kPrefix) == 0) label.erase(0, kPrefix.size());
		return label;
	}
	return id;
}

void FillSentFolderList(HWND hDlg)
{
	HWND hList = GetDlgItem(hDlg, IDC_SENT_FOLDERS);
	int previousSel = (int)SendMessageW(hList, LB_GETCURSEL, 0, 0);
	SendMessageW(hList, LB_RESETCONTENT, 0, 0);

	// Measured as rows are added so the horizontal extent can be set below.
	HDC hdc = GetDC(hList);
	HFONT hOldFont = (HFONT)SelectObject(hdc, (HFONT)SendMessageW(hList, WM_GETFONT, 0, 0));
	int widest = 0;

	s_sentFolders = LoadSentFolders();
	for (const SentFolderInfo& f : s_sentFolders) {
		LPCSTR lpDisplay = bka.GetFolderDisplayName(f.folderId.c_str());
		std::wstring row = Utf8ToWide(AnsiToUtf8(
			std::string((lpDisplay && *lpDisplay) ? lpDisplay : f.folderId.c_str())));
		if (f.learnedAt.empty()) {
			row += L"  -  未学習";
		} else {
			wchar_t suffix[192];
			// Two spaces at every field boundary, including after 学習済み.
			// The single space inside the timestamp is then the only narrow
			// gap, which is right: it joins one field rather than separating
			// two.
			swprintf(suffix, 192, L"  -  学習済み  %s  %d通  %s",
				Utf8ToWide(f.learnedAt).c_str(), f.sampleCount,
				f.learnedModel.empty() ? L"モデル不明"
				                       : Utf8ToWide(ShortModelName(f.learnedModel)).c_str());
			row += suffix;
		}
		SIZE sz{};
		if (GetTextExtentPoint32W(hdc, row.c_str(), (int)row.size(), &sz) && sz.cx > widest) {
			widest = sz.cx;
		}
		SendMessageW(hList, LB_ADDSTRING, 0, (LPARAM)row.c_str());
	}

	SelectObject(hdc, hOldFont);
	ReleaseDC(hList, hdc);

	// A folder with a long display name can still outrun the box even after
	// the row text was tightened, and widening the dialog for that one case
	// would leave the rest of it stretched. The scrollbar only appears when
	// the extent actually exceeds the client width, so the usual case is
	// unaffected.
	SendMessageW(hList, LB_SETHORIZONTALEXTENT, (WPARAM)(widest + 8), 0);

	// Keep the row the user was acting on across a refresh, but never select
	// anything on our own: both buttons here destroy data, so the user has to
	// name the target first.
	if (previousSel >= 0 && previousSel < (int)s_sentFolders.size()) {
		SendMessageW(hList, LB_SETCURSEL, previousSel, 0);
	}
}

// `text` is UTF-8, like every other string in this project. It was ANSI
// once, which is why the box spoke English: a Japanese literal compiled with
// /utf-8 would have come out as mojibake through SetDlgItemTextA.
// The only consumer of IsLicenseKeyValid. Wording kept flat on purpose: the
// dialog plays shareware straight, and the README delivers the punchline.
//
// Registered: the key field is locked and the button reads 変更. Pressing
// 変更 unlocks the field and the button reads 登録 again - exactly the cycle
// the shareware of the day had, for entering a different key. There is no
// different key. The cycle is there because it was there.
void ApplyLicenseUi(HWND hDlg, const std::string& key)
{
	bool registered = IsLicenseKeyValid(key);
	SetDlgItemTextW(hDlg, IDC_LICENSE_STATUS, registered ? L"状態: 登録済み" : L"状態: 未登録");
	SendDlgItemMessageW(hDlg, IDC_LICENSE_KEY, EM_SETREADONLY, registered ? TRUE : FALSE, 0);
	SetDlgItemTextW(hDlg, IDC_LICENSE_REGISTER, registered ? L"変更" : L"登録");
}

bool LicenseFieldLocked(HWND hDlg)
{
	return (GetWindowLongW(GetDlgItem(hDlg, IDC_LICENSE_KEY), GWL_STYLE) & ES_READONLY) != 0;
}

void SetTestResultText(HWND hDlg, const std::string& text)
{
	if (!IsWindow(hDlg)) return;
	SetDlgItemTextW(hDlg, IDC_TEST_RESULT, Utf8ToWide(text).c_str());
	// The edit control hides its scrollbar after a text change even when
	// content overflows; force it back so it is always visible.
	ShowScrollBar(GetDlgItem(hDlg, IDC_TEST_RESULT), SB_VERT, TRUE);
}

// True if `candidate` is exactly `base`, or `base-<digits>` (a dated
// snapshot id like claude-haiku-4-5-20251001).
bool ModelIdMatches(const std::string& base, const std::string& candidate)
{
	if (candidate == base) return true;
	std::string dashed = base + "-";
	if (candidate.size() > dashed.size() && candidate.compare(0, dashed.size(), dashed) == 0) {
		std::string suffix = candidate.substr(dashed.size());
		return suffix.find_first_not_of("0123456789") == std::string::npos;
	}
	return false;
}

bool IsHardcodedModel(const std::string& id)
{
	for (int i = 0; i < kModelCount; i++) {
		if (ModelIdMatches(kModels[i].id, id)) return true;
	}
	return false;
}

// Turns a GET /v1/models result into the status-box message: key validity,
// whether the selected model is available to the account (tolerating dated
// snapshot ids), any models newer than the newest one we ship hardcoded
// (ISO-8601 strings sort chronologically), and the full returned id list
// for reference/reporting.
std::string FormatCheckResult(const ModelsResponse& r, const std::string& selectedModel)
{
	if (!r.success) {
		return "エラー: " + r.error;
	}

	std::string matchedId;
	std::string newestKnown;
	for (const auto& m : r.models) {
		if (matchedId.empty() && ModelIdMatches(selectedModel, m.id)) matchedId = m.id;
		if (IsHardcodedModel(m.id) && m.createdAt > newestKnown) newestKnown = m.createdAt;
	}

	// Sentences in Japanese; model ids verbatim. The ids are what would be
	// quoted in a bug report or looked up in Anthropic's documentation, and
	// they have no Japanese form.
	std::string out = "OK: API キーは有効です。\r\n";
	if (!matchedId.empty()) {
		out += "モデル '" + selectedModel + "' は利用できます";
		if (matchedId != selectedModel) out += "（'" + matchedId + "' として）";
		out += "。";
	} else {
		out += "警告: モデル '" + selectedModel + "' はこのアカウントのモデル一覧にありません。";
	}

	if (!newestKnown.empty()) {
		std::string newer;
		for (const auto& m : r.models) {
			if (m.createdAt > newestKnown && !IsHardcodedModel(m.id)) {
				if (!newer.empty()) newer += ", ";
				newer += m.id;
				if (!m.displayName.empty()) newer += " (" + m.displayName + ")";
			}
		}
		if (!newer.empty()) out += "\r\nより新しいモデルがあります: " + newer;
	}

	std::string all;
	for (const auto& m : r.models) {
		if (!all.empty()) all += ", ";
		all += m.id;
	}
	out += "\r\n利用可能なモデル (" + std::to_string(r.models.size()) + "): " + all;

	return out;
}

INT_PTR CALLBACK DlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
	switch (msg) {
	case WM_INITDIALOG: {
		for (int i = 0; i < kModelCount; i++) {
			SendDlgItemMessageA(hDlg, IDC_MODEL, CB_ADDSTRING, 0, (LPARAM)kModels[i].label);
		}
		// Widen only the dropped-down list so the full descriptions are
		// readable; the closed combo keeps its dialog-layout width.
		SendDlgItemMessageA(hDlg, IDC_MODEL, CB_SETDROPPEDWIDTH, 380, 0);

		PluginSettings settings;
		LoadSettings(settings);
		int sel = 0;
		for (int i = 0; i < kModelCount; i++) {
			if (settings.model == kModels[i].id) {
				sel = i;
				break;
			}
		}
		SendDlgItemMessageA(hDlg, IDC_MODEL, CB_SETCURSEL, sel, 0);
		s_lastModelSel = sel;

		// If a key is stored, show a fixed-length masked placeholder (so its
		// real length isn't leaked). If not, the field is empty and the user
		// will type a fresh key - show that as plain text.
		s_keyPlaceholderShown = HasApiKey();
		if (s_keyPlaceholderShown) {
			SetDlgItemTextA(hDlg, IDC_APIKEY, kKeyPlaceholder);
		} else {
			SendDlgItemMessageW(hDlg, IDC_APIKEY, EM_SETPASSWORDCHAR, 0, 0);
		}
		SetWindowSubclass(GetDlgItem(hDlg, IDC_APIKEY), ApiKeyEditProc, 0, 0);
		EnableSelectAllShortcut(GetDlgItem(hDlg, IDC_TEST_RESULT));

		SetDlgItemInt(hDlg, IDC_SUMMARIZE_CACHE_MINUTES, (UINT)settings.summarizeCacheMinutes, FALSE);
		SetDlgItemInt(hDlg, IDC_TRANSLATE_CACHE_MINUTES, (UINT)settings.translateCacheMinutes, FALSE);
		SetDlgItemInt(hDlg, IDC_LEARN_MAX_MAILS, (UINT)settings.learnMaxMails, FALSE);
		SetDlgItemInt(hDlg, IDC_LEARN_MAX_MONTHS, (UINT)settings.learnMaxMonths, FALSE);
		CheckDlgButton(hDlg, IDC_WARN_MISSING_ATTACHMENT,
			settings.warnMissingAttachment ? BST_CHECKED : BST_UNCHECKED);
		SetDlgItemTextA(hDlg, IDC_LICENSE_KEY, settings.licenseKey.c_str());
		ApplyLicenseUi(hDlg, settings.licenseKey);
		FillSentFolderList(hDlg);

		s_keyDirty = false; // clears the EN_CHANGE from our own SetDlgItemText above

		// Focus the key field ourselves (no select-all) instead of letting
		// the dialog manager focus + highlight it.
		SetFocus(GetDlgItem(hDlg, IDC_APIKEY));
		return FALSE;
	}
	case WM_MEASUREITEM: {
		MEASUREITEMSTRUCT* mis = (MEASUREITEMSTRUCT*)lParam;
		if (mis->CtlID == IDC_MODEL) {
			// Measure with the dialog's own font (the combo's isn't set yet
			// here); using the default system font makes the row too tall.
			HDC hdc = GetDC(hDlg);
			HFONT hf = (HFONT)SendMessageW(hDlg, WM_GETFONT, 0, 0);
			HFONT old = hf ? (HFONT)SelectObject(hdc, hf) : NULL;
			TEXTMETRICW tm{};
			GetTextMetricsW(hdc, &tm);
			if (old) SelectObject(hdc, old);
			ReleaseDC(hDlg, hdc);
			mis->itemHeight = (tm.tmHeight > 0 ? tm.tmHeight : 12) + 2;
			return TRUE;
		}
		break;
	}
	case WM_DRAWITEM: {
		DRAWITEMSTRUCT* dis = (DRAWITEMSTRUCT*)lParam;
		if (dis->CtlID == IDC_MODEL && (int)dis->itemID >= 0) {
			// CB_GETLBTEXT takes no buffer size - it writes the whole item
			// however long it is - so the length has to be asked for first.
			// The items here are our own hard-coded model labels and would
			// have fit a fixed buffer, but a control that grows an item later
			// would then overwrite the stack with no warning.
			std::string text;
			int len = (int)SendMessageA(dis->hwndItem, CB_GETLBTEXTLEN, dis->itemID, 0);
			if (len > 0) {
				text.resize((size_t)len + 1);
				int got = (int)SendMessageA(dis->hwndItem, CB_GETLBTEXT, dis->itemID, (LPARAM)&text[0]);
				text.resize((got > 0 && got <= len) ? (size_t)got : 0);
			}
			if (dis->itemState & ODS_COMBOBOXEDIT) {
				// Closed combo: model name only (before " - ").
				size_t p = text.find(" - ");
				if (p != std::string::npos) text.erase(p);
			}

			bool sel = (dis->itemState & ODS_SELECTED) != 0;
			FillRect(dis->hDC, &dis->rcItem,
				GetSysColorBrush(sel ? COLOR_HIGHLIGHT : COLOR_WINDOW));
			SetBkColor(dis->hDC, GetSysColor(sel ? COLOR_HIGHLIGHT : COLOR_WINDOW));
			SetTextColor(dis->hDC, GetSysColor(sel ? COLOR_HIGHLIGHTTEXT : COLOR_WINDOWTEXT));

			HFONT hf = (HFONT)SendMessageW(dis->hwndItem, WM_GETFONT, 0, 0);
			HFONT old = hf ? (HFONT)SelectObject(dis->hDC, hf) : NULL;
			RECT rc = dis->rcItem;
			rc.left += 4;
			DrawTextA(dis->hDC, text.c_str(), -1, &rc,
				DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_NOPREFIX | DT_END_ELLIPSIS);
			if (old) SelectObject(dis->hDC, old);

			if (dis->itemState & ODS_FOCUS) DrawFocusRect(dis->hDC, &dis->rcItem);
			return TRUE;
		}
		break;
	}
	case WM_COMMAND:
		switch (LOWORD(wParam)) {
		case IDC_APIKEY:
			// Any edit invalidates a prior check result and marks the key
			// as changed (so it gets saved).
			if (HIWORD(wParam) == EN_CHANGE) {
				s_keyDirty = true;
				SetTestResultText(hDlg, "");
			}
			break;

		case IDC_LICENSE_REGISTER:
			if (HIWORD(wParam) == BN_CLICKED) {
				if (LicenseFieldLocked(hDlg)) {
					// 変更: open the field for a new key. Nothing is saved or
					// judged until 登録 is pressed again; closing the dialog
					// now leaves the existing registration as it was.
					SendDlgItemMessageW(hDlg, IDC_LICENSE_KEY, EM_SETREADONLY, FALSE, 0);
					SetDlgItemTextW(hDlg, IDC_LICENSE_REGISTER, L"登録");
					SetFocus(GetDlgItem(hDlg, IDC_LICENSE_KEY));
					SendDlgItemMessageW(hDlg, IDC_LICENSE_KEY, EM_SETSEL, 0, -1);
					return TRUE;
				}
				// 登録: takes effect on the button, not as you type - a button
				// that changed nothing would give the game away - and is saved
				// at once, so it survives キャンセル, as registration did. This
				// is the only place the key is written; OK does not touch it.
				char keyBuf[256] = {};
				GetDlgItemTextA(hDlg, IDC_LICENSE_KEY, keyBuf, sizeof(keyBuf));
				PluginSettings settings;
				LoadSettings(settings);
				settings.licenseKey = keyBuf;
				SaveSettings(settings);
				ApplyLicenseUi(hDlg, keyBuf);
			}
			return TRUE;

		case IDC_MODEL:
			// A prior check result was for the old model selection - but
			// only clear it if the selection actually changed.
			if (HIWORD(wParam) == CBN_SELCHANGE) {
				int cur = (int)SendDlgItemMessageA(hDlg, IDC_MODEL, CB_GETCURSEL, 0, 0);
				if (cur != s_lastModelSel) {
					s_lastModelSel = cur;
					SetTestResultText(hDlg, "");
				}
			}
			break;

		case IDC_TEST_SEND:
			if (HIWORD(wParam) == BN_CLICKED) {
				std::string apiKey;
				if (s_keyDirty) {
					char keyBuf[512];
					GetDlgItemTextA(hDlg, IDC_APIKEY, keyBuf, sizeof(keyBuf));
					apiKey = keyBuf;
				} else {
					LoadApiKey(apiKey); // check the stored key
				}
				if (apiKey.empty()) {
					SetTestResultText(hDlg, "先に API キーを入力してください。");
					return TRUE;
				}

				int sel = (int)SendDlgItemMessageA(hDlg, IDC_MODEL, CB_GETCURSEL, 0, 0);
				if (sel < 0 || sel >= kModelCount) sel = 0;
				std::string selectedModel = kModels[sel].id;

				EnableWindow(GetDlgItem(hDlg, IDC_TEST_SEND), FALSE);
				SetTestResultText(hDlg, "確認しています...");

				auto resp = std::make_shared<ModelsResponse>();
				AsyncRunner_Post(
					[resp, apiKey]() { *resp = ListModelsSync(apiKey); },
					[hDlg, resp, selectedModel]() {
						if (IsWindow(hDlg)) {
							EnableWindow(GetDlgItem(hDlg, IDC_TEST_SEND), TRUE);
						}
						SetTestResultText(hDlg, FormatCheckResult(*resp, selectedModel));
					});
			}
			return TRUE;

		case IDC_DELETE_PROFILE:
			if (HIWORD(wParam) == BN_CLICKED) {
				int sel = (int)SendDlgItemMessageW(hDlg, IDC_SENT_FOLDERS, LB_GETCURSEL, 0, 0);
				if (sel < 0 || sel >= (int)s_sentFolders.size()) {
					SetTestResultText(hDlg, "先に一覧からメールボックスを選択してください。");
					return TRUE;
				}
				if (s_sentFolders[sel].learnedAt.empty()) {
					SetTestResultText(hDlg, "このメールボックスはまだ学習していません。");
					return TRUE;
				}
				// Names both artifacts. One learning run produces a style
				// profile and a phrase list, and this button removes both -
				// which "学習データ" on its own does not convey to someone
				// deciding whether to press it.
				if (MessageBoxW(hDlg,
						L"選択したメールボックスの学習データを削除します。\r\n"
						L"文体の学習と、添付忘れ確認の表現リストの両方が削除されます。\r\n"
						L"フォルダの登録は残ります。よろしいですか?",
						L"Becky! Claude Plugin",
						MB_OKCANCEL | MB_ICONWARNING | MB_DEFBUTTON2) != IDOK) {
					return TRUE;
				}

				std::string mailboxId = MailboxIdFromFolderId(s_sentFolders[sel].folderId);
				if (!DeleteStyleProfile(mailboxId)) {
					SetTestResultText(hDlg, "エラー: 学習データを削除できませんでした。");
					return TRUE;
				}
				// Both files come from the same learning run and are stale
				// together; leaving the phrase list behind would go on
				// warning about a mailbox the user has just cleared.
				DeleteAttachPhrases(mailboxId);
				// Clear the stamp too, so the row goes back to "未学習".
				std::vector<SentFolderInfo> folders = s_sentFolders;
				folders[sel].learnedAt.clear();
				folders[sel].sampleCount = 0;
				SaveSentFolders(folders);
				FillSentFolderList(hDlg);
				SetTestResultText(hDlg, "このメールボックスの学習データを削除しました。");
			}
			return TRUE;

		case IDC_UNREGISTER_SELECTED:
			if (HIWORD(wParam) == BN_CLICKED) {
				int sel = (int)SendDlgItemMessageW(hDlg, IDC_SENT_FOLDERS, LB_GETCURSEL, 0, 0);
				if (sel < 0 || sel >= (int)s_sentFolders.size()) {
					SetTestResultText(hDlg, "先に一覧からメールボックスを選択してください。");
					return TRUE;
				}
				if (MessageBoxW(hDlg,
						L"選択したメールボックスの学習フォルダを解除します。\r\n"
						L"文体の学習と、添付忘れ確認の表現リストも削除されます。\r\n"
						L"よろしいですか?",
						L"Becky! Claude Plugin",
						MB_OKCANCEL | MB_ICONWARNING | MB_DEFBUTTON2) != IDOK) {
					return TRUE;
				}
				// An unregistered mailbox's profile is unreachable, so drop it
				// as well rather than leave a stale file to be resurrected.
				DeleteStyleProfile(MailboxIdFromFolderId(s_sentFolders[sel].folderId));
				DeleteAttachPhrases(MailboxIdFromFolderId(s_sentFolders[sel].folderId));
				std::vector<SentFolderInfo> folders = s_sentFolders;
				folders.erase(folders.begin() + sel);
				SaveSentFolders(folders);
				FillSentFolderList(hDlg);
				// The row is gone; keeping the index would silently select a
				// different mailbox for the next button press.
				SendDlgItemMessageW(hDlg, IDC_SENT_FOLDERS, LB_SETCURSEL, (WPARAM)-1, 0);
				SetTestResultText(hDlg, "学習フォルダを解除し、学習データも削除しました。");
			}
			return TRUE;

		case IDC_CLEAR_SUMMARIZE_CACHE:
		case IDC_CLEAR_TRANSLATE_CACHE:
			if (HIWORD(wParam) == BN_CLICKED) {
				bool isSummarize = (LOWORD(wParam) == IDC_CLEAR_SUMMARIZE_CACHE);
				const char* label = isSummarize ? "要約" : "翻訳";
				int n = ClearCache(isSummarize ? "summarize" : "translate");
				std::string text;
				if (n < 0) {
					text = std::string("エラー: ") + label + "キャッシュを削除できませんでした。";
				} else {
					text = std::string(label) + "キャッシュを削除しました（" + std::to_string(n) + " 件）。";
				}
				SetTestResultText(hDlg, text);
			}
			return TRUE;

		case IDOK: {
			// Close only when the OK button itself is the source (a real
			// click, or keyboard-activated while focused). Enter pressed in
			// any other field also routes here via the dialog manager's
			// default command - ignore that so it doesn't close/save.
			if (GetFocus() != GetDlgItem(hDlg, IDOK)) return TRUE;

			int sel = (int)SendDlgItemMessageA(hDlg, IDC_MODEL, CB_GETCURSEL, 0, 0);
			if (sel < 0 || sel >= kModelCount) sel = 0;

			PluginSettings settings;
			LoadSettings(settings);
			settings.model = kModels[sel].id;
			settings.summarizeCacheMinutes = (int)GetDlgItemInt(hDlg, IDC_SUMMARIZE_CACHE_MINUTES, NULL, FALSE);
			settings.translateCacheMinutes = (int)GetDlgItemInt(hDlg, IDC_TRANSLATE_CACHE_MINUTES, NULL, FALSE);

			// A zero or absurd sample size would make learning useless rather
			// than merely cheap, so clamp instead of trusting the field.
			int maxMails = (int)GetDlgItemInt(hDlg, IDC_LEARN_MAX_MAILS, NULL, FALSE);
			if (maxMails < 10) maxMails = 10;
			if (maxMails > 1000) maxMails = 1000;
			settings.learnMaxMails = maxMails;

			int maxMonths = (int)GetDlgItemInt(hDlg, IDC_LEARN_MAX_MONTHS, NULL, FALSE);
			if (maxMonths < 0) maxMonths = 0;
			if (maxMonths > 600) maxMonths = 600;
			settings.learnMaxMonths = maxMonths;

			settings.warnMissingAttachment =
				IsDlgButtonChecked(hDlg, IDC_WARN_MISSING_ATTACHMENT) == BST_CHECKED;

			// settings.licenseKey is left as loaded: only the 登録 button
			// writes it.
			SaveSettings(settings);

			if (s_keyDirty) {
				char keyBuf[512];
				GetDlgItemTextA(hDlg, IDC_APIKEY, keyBuf, sizeof(keyBuf));
				if (keyBuf[0] != '\0') {
					SaveApiKey(keyBuf);
				}
			}
			EndDialog(hDlg, IDOK);
			return TRUE;
		}
		case IDCANCEL:
			EndDialog(hDlg, IDCANCEL);
			return TRUE;
		}
		break;
	}
	return FALSE;
}

} // namespace

void ShowSettingsDialog(HWND hParent)
{
	DialogBoxParamA(g_hInstance, MAKEINTRESOURCEA(IDD_SETTINGS), hParent, DlgProc, 0);
}

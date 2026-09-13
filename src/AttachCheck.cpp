#include <windows.h>
#include <string>
#include <vector>
#include "AttachCheck.h"
#include "resource.h"
#include "BeckyApi.h"
#include "Settings.h"
#include "Encoding.h"
#include "MailSource.h"
#include "PluginUtil.h"

extern CBeckyAPI bka;
extern HINSTANCE g_hInstance;

// --- forgotten-attachment warning ---------------------------------------
//
// The one place this plug-in can stop something the user asked for, so the
// whole design leans one way: every uncertainty resolves to letting the send
// through. Nothing is cancelled by us - the dialog asks, and the send is
// cancelled only if the user says to.
//
// No API call and no network wait on this path. Everything it needs was
// worked out at learning time; here it is a substring search.

namespace {

// Used ONLY until a list has been learned for the mailbox - see below.
// Kept to first-person declarations of the act itself, and deliberately not
// extended into 「ご確認ください」 territory: phrases that merely often
// accompany an attachment are the ones that warn about a mail that was fine,
// and this list cannot be edited away by the user the way the learned one can.
const char* kBuiltinAttachPhrases[] = {
	"添付します",
	"添付いたします",
	"添付しました",
	"添付いたしました",
};

INT_PTR CALLBACK AttachWarnDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
	switch (msg) {
	case WM_INITDIALOG:
		SetDlgItemTextW(hDlg, IDC_ATTACH_PHRASE, (const wchar_t*)lParam);
		// Focus the safe answer explicitly. It sits on the right, so the
		// dialog manager's own choice - the first button in tab order - would
		// be the one that sends; and a focused push button takes Enter for
		// itself regardless of which one carries BS_DEFPUSHBUTTON.
		SetFocus(GetDlgItem(hDlg, IDCANCEL));
		return FALSE;
	case WM_COMMAND:
		if (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL) {
			EndDialog(hDlg, LOWORD(wParam));
			return TRUE;
		}
		break;
	}
	return FALSE;
}

} // namespace

int CheckMissingAttachment(HWND hWnd, int nMode)
{
	// Only an actual send. Saving a draft or setting a reminder is exactly
	// when a mail is legitimately unfinished.
	if (nMode != 0) return 0;

	PluginSettings settings;
	LoadSettings(settings);
	if (!settings.warnMissingAttachment) return 0;

	LPSTR src = bka.CompGetSource(hWnd);
	if (!src) return 0;
	std::string rawSource = src;
	bka.Free(src);
	if (rawSource.empty()) return 0;

	// Positive confirmation that a file IS attached ends it here. The check
	// only ever proceeds on a source we could read and that demonstrably
	// carries no attachment.
	if (SourceHasAttachment(rawSource)) return 0;

	std::string bodyUtf8 = ExtractPlainTextBodyUtf8(rawSource);
	if (bodyUtf8.empty()) return 0;

	// Only what the user wrote: not the quoted original (whose author may
	// well have said 「添付します」 about a file of their own), and not the
	// signature.
	size_t tailAt = FindPreservedTailStart(bodyUtf8, LoadAllSignatures());
	std::string written = (tailAt == std::string::npos) ? bodyUtf8 : bodyUtf8.substr(0, tailAt);
	written = StripQuotedLines(StripForwardedTail(written));
	if (written.empty()) return 0;

	// The built-in phrases are a fallback, not a supplement. Learning tests
	// every candidate against this mailbox's own mail and keeps only what
	// never appears without an attachment; on both mailboxes tried, that test
	// rejected all four built-ins - they do turn up in mails with nothing
	// attached ("後ほど添付します" and the like). Adding them back on top of a
	// list that was measured would put back exactly the false warnings the
	// measurement removed.
	std::vector<std::string> phrases;
	if (!LoadAttachPhrases(MailboxIdForCompose(hWnd), phrases)) {
		for (const char* p : kBuiltinAttachPhrases) phrases.push_back(p);
	}

	std::string hit;
	for (const std::string& p : phrases) {
		if (!p.empty() && written.find(p) != std::string::npos) {
			hit = p;
			break;
		}
	}
	if (hit.empty()) return 0;

	std::wstring shown = L"「" + Utf8ToWide(hit) + L"」";
	INT_PTR r = DialogBoxParamW(g_hInstance, MAKEINTRESOURCEW(IDD_ATTACHWARN),
		hWnd, AttachWarnDlgProc, (LPARAM)shown.c_str());
	// Anything other than an explicit 中止 - including the dialog failing to
	// open at all - lets the send proceed.
	return (r == IDCANCEL) ? -1 : 0;
}

#include <windows.h>
#include <string>
#include "Encoding.h"
#include "BeckyApi.h"
#include "AsyncRunner.h"
#include "SettingsDialog.h"
#include "Version.h"
#include "MailActions.h"
#include "ComposeActions.h"
#include "AttachCheck.h"
#include "LearnStyle.h"

CBeckyAPI bka;

HINSTANCE g_hInstance = NULL;

// The SDK template derives an ini path next to the DLL here. This plug-in
// keeps its settings under bka.GetDataFolder() instead - the DLL lives in
// Program Files, which a normal user cannot write to - so the path was
// computed and never read. It is gone rather than merely unused: it was the
// last strcpy/strcat in the project, and both were unbounded writes into a
// MAX_PATH+2 buffer that GetModuleFileNameA can already have filled.
BOOL APIENTRY DllMain(HANDLE hModule, DWORD ulReasonForCall, LPVOID /*lpReserved*/)
{
	g_hInstance = (HINSTANCE)hModule;
	switch (ulReasonForCall) {
	case DLL_PROCESS_ATTACH:
		if (!bka.InitAPI()) {
			return FALSE;
		}
		break;
	}
	return TRUE;
}

namespace {

// The ツール submenu of a menu bar, found by its caption rather than by
// position. Becky!'s bar layout is not part of the plug-in API, and a fixed
// index would silently land the items in the wrong menu after any change to
// it; a caption match either finds the right menu or adds nothing - and the
// right-click menus stay either way. The caption carries an accelerator
// ("ツール(&T)"), so match on the prefix.
HMENU FindToolsMenu(HMENU hBar)
{
	int count = GetMenuItemCount(hBar);
	for (int i = 0; i < count; i++) {
		wchar_t caption[64] = {};
		GetMenuStringW(hBar, (UINT)i, caption, 64, MF_BYPOSITION);
		if (wcsncmp(caption, L"ツール", 3) == 0) return GetSubMenu(hBar, i);
	}
	return NULL;
}

// Mail-reading actions: on the list's right-click menu and, for the sake of
// being findable, on the main window's ツール menu as well. Both act on the
// mail currently shown, so the entry point makes no difference to the result.
void AppendMailActions(HMENU hMenu, int nType)
{
	UINT nID1 = bka.RegisterCommand("Summarize current mail with Claude", nType, OnCmdSummarize);
	UINT nID3 = bka.RegisterCommand("Translate current mail to Japanese with Claude", nType, OnCmdTranslateMailToJapanese);
	AppendMenuA(hMenu, MF_SEPARATOR, 0, NULL);
	AppendMenuW(hMenu, MF_STRING, nID1, L"Claude でメールを要約");
	AppendMenuW(hMenu, MF_STRING, nID3, L"Claude でメールを翻訳(日)");
}

// Compose actions: on the editor's right-click menu and on the compose
// window's ツール menu, which is where Becky! itself keeps スペルチェック.
void AppendComposeActions(HMENU hMenu, int nType)
{
	UINT nID1 = bka.RegisterCommand("Draft a reply with Claude", nType, OnCmdDraftReply);
	UINT nID2 = bka.RegisterCommand("Proofread what you wrote with Claude", nType, OnCmdProofread);
	UINT nID3 = bka.RegisterCommand("Turn what you wrote into English with Claude", nType, OnCmdTranslateEnglish);
	AppendMenuA(hMenu, MF_SEPARATOR, 0, NULL);
	AppendMenuW(hMenu, MF_STRING, nID1, L"Claude で本文を下書き");
	AppendMenuW(hMenu, MF_STRING, nID2, L"Claude で本文を校正");
	AppendMenuW(hMenu, MF_STRING, nID3, L"Claude で本文を翻訳(英)");
}

} // namespace

extern "C" {

int WINAPI BKC_OnStart()
{
	AsyncRunner_Init(g_hInstance);
	return 0;
}

int WINAPI BKC_OnExit()
{
	return 0;
}

int WINAPI BKC_OnMenuInit(HWND /*hWnd*/, HMENU hMenu, int nType)
{
	switch (nType) {
	case BKC_MENU_TREEVIEW: {
		// Registering a Sent folder means pointing at one, so it belongs on
		// the folder itself rather than taking permanent space in Tools.
		// Offer exactly one of the two actions: learning on the folder that
		// would actually be read, registering on anything else. Every folder
		// in the tree carries this menu, and only one line is ever useful.
		AppendMenuA(hMenu, MF_SEPARATOR, 0, NULL);
		if (CurrentFolderIsLearnTarget()) {
			UINT nID = bka.RegisterCommand("Learn writing style from this Sent folder with Claude", nType, OnCmdLearnStyle);
			AppendMenuW(hMenu, MF_STRING, nID, L"Claude で送信済みメールを学習");
		} else {
			UINT nID = bka.RegisterCommand("Set this folder as the mailbox's learning source", nType, OnCmdRegisterSentFolder);
			AppendMenuW(hMenu, MF_STRING, nID, L"Claude で学習フォルダとして指定");
		}
		break;
	}
	case BKC_MENU_LISTVIEW:
		AppendMailActions(hMenu, nType);
		break;
	case BKC_MENU_COMPEDIT:
		AppendComposeActions(hMenu, nType);
		break;
	// The two menu bars. Here hMenu is the whole bar, not a popup, so the
	// items go into its ツール submenu. Learning is deliberately absent: its
	// target is "the folder you right-clicked", and a bar item would act on
	// whatever folder happens to be current, which the user is rarely
	// thinking about.
	case BKC_MENU_MAIN: {
		HMENU hTools = FindToolsMenu(hMenu);
		if (hTools) AppendMailActions(hTools, nType);
		break;
	}
	case BKC_MENU_COMPOSE: {
		HMENU hTools = FindToolsMenu(hMenu);
		if (hTools) AppendComposeActions(hTools, nType);
		break;
	}
	default:
		break;
	}
	return 0;
}

int WINAPI BKC_OnOpenFolder(LPCTSTR /*lpFolderID*/)
{
	return 0;
}

int WINAPI BKC_OnOpenMail(LPCTSTR /*lpMailID*/)
{
	return 0;
}

int WINAPI BKC_OnEveryMinute()
{
	return 0;
}

int WINAPI BKC_OnOpenCompose(HWND /*hWnd*/, int /*nMode*/)
{
	return 0;
}

// Returning anything but 0 cancels the operation, so this is the one callback
// that can lose a mail the user meant to send. It delegates to a check whose
// every failure path returns 0.
int WINAPI BKC_OnOutgoing(HWND hWnd, int nMode)
{
	return CheckMissingAttachment(hWnd, nMode);
}

int WINAPI BKC_OnKeyDispatch(HWND /*hWnd*/, int /*nKey*/, int /*nShift*/)
{
	return 0;
}

int WINAPI BKC_OnRetrieve(LPCTSTR /*lpMessage*/, LPCTSTR /*lpMailID*/)
{
	return 0;
}

int WINAPI BKC_OnSend(LPCTSTR /*lpMessage*/)
{
	return 0;
}

int WINAPI BKC_OnFinishRetrieve(int /*nNumber*/)
{
	return 0;
}

int WINAPI BKC_OnPlugInSetup(HWND hWnd)
{
	ShowSettingsDialog(hWnd);
	return 1;
}

typedef struct tagBKPLUGININFO
{
	char szPlugInName[80];
	char szVendor[80];
	char szVersion[80];
	char szDescription[256];
} BKPLUGININFO, *LPBKPLUGININFO;

// Becky! shows these when the plug-in is first loaded, and shows only the
// name and this description - so the description is the one place a user has
// to decide whether to install it. It therefore states the two things that
// matter for that decision: that an Anthropic API key is required, and that
// mail bodies are sent to Anthropic.
int WINAPI BKC_OnPlugInInfo(LPBKPLUGININFO lpPlugInInfo)
{
	// These fields are read in the system codepage, but the sources compile
	// with /utf-8 - a Japanese literal copied straight in would display as
	// mojibake. lstrcpynA also bounds the copy, which plain strcpy did not.
	auto setField = [](char* dst, int size, const char* utf8) {
		std::string ansi = Utf8ToAnsi(utf8);
		(void)lstrcpynA(dst, ansi.c_str(), size); // truncation is the intended bound
	};

	setField(lpPlugInInfo->szPlugInName, sizeof(lpPlugInInfo->szPlugInName),
		"Becky! Claude Plugin");
	setField(lpPlugInInfo->szVendor, sizeof(lpPlugInInfo->szVendor),
		"Morimoto, Kenji");
	setField(lpPlugInInfo->szVersion, sizeof(lpPlugInInfo->szVersion),
		BKCLAUDE_VERSION_STRING);
	setField(lpPlugInInfo->szDescription, sizeof(lpPlugInInfo->szDescription),
		"Claude AIでメールの要約・翻訳・返信下書き・校正を行います。"
		"利用にはAnthropicのAPIキーが必要で、対象メールの本文がAnthropicへ送信されます。");
	return 0;
}

int WINAPI BKC_OnDragDrop(LPCSTR /*lpTgt*/, LPCSTR /*lpSrc*/, int /*nCount*/, int /*dropEffect*/)
{
	return 0;
}

int WINAPI BKC_OnBeforeFilter2(LPCSTR /*lpMessage*/, LPCSTR /*lpMailBox*/, int* /*lpnAction*/, char** /*lppParam*/)
{
	return BKC_FILTER_DEFAULT;
}

} // extern "C"

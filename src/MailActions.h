#pragma once
#include <windows.h>

// The two actions on an existing mail (list-view context menu). Both read the
// current mail, show a result dialog, and never modify the mail.
void CALLBACK OnCmdSummarize(HWND hWnd, LPARAM lParam);
void CALLBACK OnCmdTranslateMailToJapanese(HWND hWnd, LPARAM lParam);

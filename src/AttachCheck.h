#pragma once
#include <windows.h>

// Called from BKC_OnOutgoing. Returns 0 to let the send proceed, or -1 to
// cancel it - and -1 only ever because the user chose 編集へ戻る in the
// warning dialog. Every other path, including every failure, returns 0: an
// unsent mail the user believes they sent is a worse outcome than a missed
// warning.
int CheckMissingAttachment(HWND hWnd, int nMode);

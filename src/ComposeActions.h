#pragma once
#include <windows.h>

// The three actions on the message being composed (compose-window context
// menu). One shared implementation; they differ only in the prompt. All of
// them work on the part of the body above the quote - what the user actually
// wrote - and re-attach the quote and signature verbatim.
void CALLBACK OnCmdDraftReply(HWND hWnd, LPARAM lParam);
void CALLBACK OnCmdProofread(HWND hWnd, LPARAM lParam);
void CALLBACK OnCmdTranslateEnglish(HWND hWnd, LPARAM lParam);

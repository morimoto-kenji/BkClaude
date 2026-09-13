#pragma once
#include <windows.h>
#include <string>

// Learning the writing style of one mailbox from its Sent folder, and - from
// the same scan - the phrases it uses to announce an attachment. The scan runs
// as a timer-driven job rather than a loop, because Becky! builds a folder's
// mail list on its own message loop; see the comments in LearnStyle.cpp.

// Folder tree context menu: remembers the right-clicked folder as the Sent
// folder of its mailbox.
void CALLBACK OnCmdRegisterSentFolder(HWND hWnd, LPARAM lParam);

// Same menu: learns from the right-clicked folder, which must already be
// registered. Registration is nearly always followed by learning, so the two
// sit side by side.
void CALLBACK OnCmdLearnStyle(HWND hWnd, LPARAM lParam);

// True when the folder the tree menu is being built for is the one learning
// would actually read - an explicit registration, or Becky!'s own Sent folder
// when nothing is registered. Lets the menu offer registering or learning,
// never both.
bool CurrentFolderIsLearnTarget();

// Runs the learning pass over one registered Sent folder.
void LearnStyleForFolder(HWND hWnd, const std::string& folderId);

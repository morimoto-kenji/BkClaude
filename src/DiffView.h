#pragma once
#include <windows.h>
#include <string>

// Renders `after` into a RichEdit control with the differences from `before`
// marked: text that survived unchanged in the normal colour, text that was
// removed struck through in red, text that was added in blue.
//
// Proofreading changes a particle here and a comma there, so showing the
// corrected text alone would leave the reader to hunt for what moved. The
// colouring exists only in this preview - what is written back to the mail
// is plain text, since a text mail cannot carry formatting.
void SetRichTextWithDiff(HWND hEdit, const std::wstring& before, const std::wstring& after);

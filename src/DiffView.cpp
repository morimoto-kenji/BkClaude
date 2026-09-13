#include <windows.h>
#include <richedit.h>
#include <vector>
#include <string>
#include "DiffView.h"

namespace {

enum SpanKind { kSame, kRemoved, kAdded };

struct Span
{
	SpanKind kind;
	std::wstring text;
};

// Longest common subsequence over two sequences, returned as an edit script.
// `Emit` is called for each element in order with its kind.
//
// The table is O(n*m) ints, so callers must bound their inputs - see the
// caps in DiffLines/DiffChars below.
template <typename T, typename Emit>
void LcsScript(const std::vector<T>& a, const std::vector<T>& b, Emit emit)
{
	size_t n = a.size(), m = b.size();
	std::vector<std::vector<int>> len(n + 1, std::vector<int>(m + 1, 0));
	for (size_t i = n; i-- > 0;) {
		for (size_t j = m; j-- > 0;) {
			len[i][j] = (a[i] == b[j]) ? len[i + 1][j + 1] + 1
			                           : (len[i + 1][j] >= len[i][j + 1] ? len[i + 1][j] : len[i][j + 1]);
		}
	}
	size_t i = 0, j = 0;
	while (i < n && j < m) {
		if (a[i] == b[j]) { emit(kSame, a[i]); i++; j++; }
		else if (len[i + 1][j] >= len[i][j + 1]) { emit(kRemoved, a[i]); i++; }
		else { emit(kAdded, b[j]); j++; }
	}
	while (i < n) { emit(kRemoved, a[i]); i++; }
	while (j < m) { emit(kAdded, b[j]); j++; }
}

void AppendSpan(std::vector<Span>& out, SpanKind kind, const std::wstring& text)
{
	if (text.empty()) return;
	if (!out.empty() && out.back().kind == kind) {
		out.back().text += text;
	} else {
		Span s;
		s.kind = kind;
		s.text = text;
		out.push_back(s);
	}
}

// Joins the hard line breaks inside each paragraph, leaving blank lines as
// paragraph separators. A mail is wrapped for a fixed-pitch ~70 column
// editor; this preview uses a proportional font at a different size, so those
// breaks land in arbitrary places and the text reads as broken - and the
// proofreader's insertions push them further out of step. Display only: what
// gets applied is the model's output with its own line breaks intact.
// Columns a string occupies in a fixed-pitch font, which is the unit the
// mail was wrapped in.
int DisplayWidth(const std::wstring& s)
{
	int w = 0;
	for (wchar_t c : s) {
		bool wide = (c >= 0x1100 && c <= 0x115F) || (c >= 0x2E80 && c <= 0xA4CF) ||
		            (c >= 0xAC00 && c <= 0xD7A3) || (c >= 0xF900 && c <= 0xFAFF) ||
		            (c >= 0xFE30 && c <= 0xFE6F) || (c >= 0xFF00 && c <= 0xFF60) ||
		            (c >= 0xFFE0 && c <= 0xFFE6);
		w += wide ? 2 : 1;
	}
	return w;
}

// Every soft line break inside a paragraph is joined, with no attempt to
// guess which were deliberate.
//
// Guessing was tried - a break after a line that reached the wrap margin is
// the margin, one after a short line is deliberate - and it broke the diff.
// The margin can only be estimated from the original; the correction comes
// from the model and is wrapped wherever the model chose. The two sides then
// unwrapped into different numbers of paragraphs, failed to line up, and a
// one-word edit was rendered as two whole paragraphs replaced.
//
// Nothing structural is lost by joining: blank lines, list markers and
// indented lines still end a paragraph, and in practice those are what
// carries a writer's intent. A soft break mid-sentence is the wrap.
//
// Built as a list of finished blocks and joined at the end, rather than
// appended to a running string: that way runs of blank lines collapse to one
// separator and the result has no stray break at either end.
std::wstring UnwrapParagraphs(const std::wstring& s)
{
	std::vector<std::wstring> blocks;
	std::wstring current; // the paragraph being accumulated

	auto flush = [&blocks, &current]() {
		if (!current.empty()) {
			blocks.push_back(current);
			current.clear();
		}
	};

	size_t start = 0;
	while (start <= s.size()) {
		size_t nl = s.find(L'\n', start);
		size_t end = (nl == std::wstring::npos) ? s.size() : nl;
		std::wstring line = s.substr(start, end - start);
		while (!line.empty() && line.back() == L'\r') line.pop_back();

		size_t firstPos = line.find_first_not_of(L" \t");
		if (firstPos == std::wstring::npos) {
			flush(); // blank line ends the paragraph; the run collapses
		} else {
			wchar_t first = line[firstPos];
			// A list item or an indented line owns its own line - joining it
			// into the previous one would destroy structure the writer put
			// there deliberately.
			bool marker = (first == L'-' || first == L'*' || first == L'・' ||
			               first == L'●' || first == L'○' || first == L'>');
			bool numbered = (first >= L'0' && first <= L'9') &&
			                firstPos + 1 < line.size() &&
			                (line[firstPos + 1] == L'.' || line[firstPos + 1] == L')' ||
			                 line[firstPos + 1] == L'。' || line[firstPos + 1] == L'）');
			bool indented = (firstPos > 0);

			if (marker || numbered || indented) {
				flush();
				blocks.push_back(line);
			} else if (current.empty()) {
				current = line;
			} else {
				// The break stood in for a space in Latin text; Japanese
				// needs nothing between the two halves.
				if (current.back() < 128 && first < 128) current += L' ';
				current += line;
			}
		}
		if (nl == std::wstring::npos) break;
		start = nl + 1;
	}
	flush();

	std::wstring out;
	for (size_t i = 0; i < blocks.size(); i++) {
		if (i) out += L"\n";
		out += blocks[i];
	}
	return out;
}

std::vector<std::wstring> SplitLines(const std::wstring& s)
{
	std::vector<std::wstring> lines;
	size_t start = 0;
	while (start <= s.size()) {
		size_t nl = s.find(L'\n', start);
		size_t end = (nl == std::wstring::npos) ? s.size() : nl;
		std::wstring line = s.substr(start, end - start);
		while (!line.empty() && line.back() == L'\r') line.pop_back();
		lines.push_back(line);
		if (nl == std::wstring::npos) break;
		start = nl + 1;
	}
	return lines;
}

// Character-level diff of one replaced line against its replacement. Skipped
// for very long lines, where the O(n*m) table would be the dominant cost and
// a whole-line replacement reads just as well.
void DiffChars(const std::wstring& before, const std::wstring& after, std::vector<Span>& out)
{
	// Peel off the identical head and tail first. A proofreading edit touches
	// a few characters in the middle of a paragraph, so this usually leaves
	// the quadratic part tiny - and keeps whole paragraphs under the cap
	// below, which is what used to force them into a wholesale replacement.
	size_t head = 0;
	while (head < before.size() && head < after.size() && before[head] == after[head]) {
		head++;
	}
	size_t tail = 0;
	while (tail < before.size() - head && tail < after.size() - head &&
	       before[before.size() - 1 - tail] == after[after.size() - 1 - tail]) {
		tail++;
	}

	AppendSpan(out, kSame, before.substr(0, head));

	std::wstring midBefore = before.substr(head, before.size() - head - tail);
	std::wstring midAfter = after.substr(head, after.size() - head - tail);

	// The DP table is n*m ints; 1200 caps it at roughly 5MB, fine
	// transiently. Beyond that, show the differing middle as a replacement.
	const size_t kCharDiffLimit = 1200;
	if (midBefore.size() > kCharDiffLimit || midAfter.size() > kCharDiffLimit) {
		AppendSpan(out, kRemoved, midBefore);
		AppendSpan(out, kAdded, midAfter);
	} else {
		std::vector<wchar_t> a(midBefore.begin(), midBefore.end());
		std::vector<wchar_t> b(midAfter.begin(), midAfter.end());
		LcsScript(a, b, [&out](SpanKind kind, wchar_t c) {
			AppendSpan(out, kind, std::wstring(1, c));
		});
	}

	AppendSpan(out, kSame, before.substr(before.size() - tail));
}

std::vector<Span> BuildDiff(const std::wstring& beforeRaw, const std::wstring& afterRaw)
{
	std::wstring before = UnwrapParagraphs(beforeRaw);
	std::wstring after = UnwrapParagraphs(afterRaw);
	std::vector<std::wstring> a = SplitLines(before);
	std::vector<std::wstring> b = SplitLines(after);

	// Line-level first: proofreading normally keeps the line structure, so
	// this isolates the handful of lines that actually changed.
	const size_t kLineDiffLimit = 400;
	std::vector<Span> out;
	if (a.size() > kLineDiffLimit || b.size() > kLineDiffLimit) {
		AppendSpan(out, kRemoved, before);
		AppendSpan(out, kAdded, after);
		return out;
	}

	std::vector<std::pair<SpanKind, std::wstring>> script;
	LcsScript(a, b, [&script](SpanKind kind, const std::wstring& line) {
		script.push_back(std::make_pair(kind, line));
	});

	// Walk the script, pairing each run of removed lines with the run of
	// added lines that follows it. A 1:1 pair is the common case and gets a
	// character-level diff; anything else is shown as whole lines.
	size_t i = 0;
	while (i < script.size()) {
		if (script[i].first == kSame) {
			AppendSpan(out, kSame, script[i].second + L"\n");
			i++;
			continue;
		}
		size_t remStart = i;
		while (i < script.size() && script[i].first == kRemoved) i++;
		size_t remEnd = i;
		size_t addStart = i;
		while (i < script.size() && script[i].first == kAdded) i++;
		size_t addEnd = i;

		size_t remCount = remEnd - remStart, addCount = addEnd - addStart;
		if (remCount == addCount) {
			for (size_t k = 0; k < remCount; k++) {
				DiffChars(script[remStart + k].second, script[addStart + k].second, out);
				AppendSpan(out, kSame, L"\n");
			}
		} else {
			// Unequal runs: concatenate each side and diff the two as text.
			// Emitting whole paragraphs as removed-then-added instead - which
			// is what this used to do - hides a small edit inside a wall of
			// red, exactly what the colouring exists to prevent.
			std::wstring rem, add;
			for (size_t k = remStart; k < remEnd; k++) {
				if (!rem.empty()) rem += L"\n";
				rem += script[k].second;
			}
			for (size_t k = addStart; k < addEnd; k++) {
				if (!add.empty()) add += L"\n";
				add += script[k].second;
			}
			DiffChars(rem, add, out);
			AppendSpan(out, kSame, L"\n");
		}
	}

	// Every line was emitted with a trailing break, so the last one leaves a
	// blank line hanging at the end of the view.
	while (!out.empty()) {
		std::wstring& tail = out.back().text;
		while (!tail.empty() && (tail.back() == L'\n' || tail.back() == L'\r')) tail.pop_back();
		if (!tail.empty()) break;
		out.pop_back();
	}
	return out;
}

} // namespace

void SetRichTextWithDiff(HWND hEdit, const std::wstring& before, const std::wstring& after)
{
	std::vector<Span> spans = BuildDiff(before, after);

	std::wstring joined;
	for (const Span& s : spans) joined += s.text;
	// RichEdit stores a paragraph break as a bare CR, so the offsets below
	// stay in step with the control only if the text uses CR alone.
	for (wchar_t& c : joined) {
		if (c == L'\n') c = L'\r';
	}
	SetWindowTextW(hEdit, joined.c_str());

	CHARFORMAT2W base{};
	base.cbSize = sizeof(base);
	base.dwMask = CFM_FACE | CFM_SIZE;
	wcscpy_s(base.szFaceName, L"Yu Gothic UI");
	base.yHeight = 200; // twips; 20 per point
	SendMessageW(hEdit, EM_SETSEL, 0, -1);
	SendMessageW(hEdit, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&base);

	long pos = 0;
	for (const Span& s : spans) {
		long len = (long)s.text.size();
		if (s.kind != kSame) {
			SendMessageW(hEdit, EM_SETSEL, pos, pos + len);
			CHARFORMAT2W cf{};
			cf.cbSize = sizeof(cf);
			cf.dwMask = CFM_COLOR | CFM_STRIKEOUT;
			cf.dwEffects = (s.kind == kRemoved) ? CFE_STRIKEOUT : 0;
			cf.crTextColor = (s.kind == kRemoved) ? RGB(192, 0, 0) : RGB(0, 80, 200);
			SendMessageW(hEdit, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&cf);
		}
		pos += len;
	}
	SendMessageW(hEdit, EM_SETSEL, 0, 0);
}

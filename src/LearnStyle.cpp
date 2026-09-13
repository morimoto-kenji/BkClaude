#include <windows.h>
#include <string>
#include <vector>
#include <memory>
#include <map>
#include <algorithm>
#include <cstdio>
#include "LearnStyle.h"
#include "resource.h"
#include "BeckyApi.h"
#include "Settings.h"
#include "AsyncRunner.h"
#include "Encoding.h"
#include "MailSource.h"
#include "PluginUtil.h"

extern CBeckyAPI bka;
extern HINSTANCE g_hInstance;

namespace {

// Becky!'s own Sent folder. Undocumented - derived from the on-disk layout,
// where every mailbox has it, IMAP and POP alike. Using it means the user
// normally never has to register anything; an explicit registration wins
// over it, for people who file sent mail somewhere of their own.
std::string DerivedSentFolderId(const std::string& mailboxId)
{
	if (mailboxId.empty()) return std::string();
	return mailboxId + "!!!!Outbox\\!!!Sent\\";
}

// The folder learning would actually read for this mailbox.
std::string EffectiveSentFolderId(const std::string& mailboxId)
{
	for (const SentFolderInfo& f : LoadSentFolders()) {
		if (MailboxIdFromFolderId(f.folderId) == mailboxId) return f.folderId;
	}
	return DerivedSentFolderId(mailboxId);
}

// Progress window for the folder scan. It is ours, and only ours: repainting
// Becky!'s own windows from inside the scan re-enters Becky! while we have
// its current folder swapped out, which kills B2.exe. Its timer also drives
// the scan - see LearnTick.
HWND s_progressDlg = NULL;
bool s_learning = false; // guards against a second overlapping run
const UINT_PTR kLearnTimerId = 1;

void LearnTick();
void LearnCancel();

INT_PTR CALLBACK ProgressDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM)
{
	if (msg == WM_TIMER && wParam == kLearnTimerId) {
		LearnTick();
		return TRUE;
	}
	if (msg == WM_COMMAND && LOWORD(wParam) == IDCANCEL) {
		EnableWindow(GetDlgItem(hDlg, IDCANCEL), FALSE);
		LearnCancel();
		return TRUE;
	}
	return FALSE;
}

void OpenProgress()
{
	if (s_progressDlg) return;
	// Owned by Becky!'s main window purely for z-order.
	//
	// All four parameters are [out] and Becky! writes through every one of
	// them unconditionally - passing NULL for the three we don't need makes
	// B2.exe store to address 0 and die with 0xC0000005. Always give it four
	// real variables.
	HWND hMain = NULL, hTree = NULL, hList = NULL, hView = NULL;
	bka.GetWindowHandles(&hMain, &hTree, &hList, &hView);
	s_progressDlg = CreateDialogW(g_hInstance, MAKEINTRESOURCEW(IDD_PROGRESS),
		hMain, ProgressDlgProc);
	if (s_progressDlg) ShowWindow(s_progressDlg, SW_SHOWNORMAL);
}

void SetProgress(const std::string& utf8)
{
	if (!s_progressDlg) return;
	SetDlgItemTextW(s_progressDlg, IDC_PROGRESS_TEXT, Utf8ToWide(utf8).c_str());
	RedrawWindow(s_progressDlg, NULL, NULL, RDW_UPDATENOW | RDW_ALLCHILDREN);
}

void CloseProgress()
{
	if (!s_progressDlg) return;
	KillTimer(s_progressDlg, kLearnTimerId);
	DestroyWindow(s_progressDlg);
	s_progressDlg = NULL;
}

// --- style learning job -------------------------------------------------
//
// Split across timer ticks rather than run as one loop. Two reasons. Becky!
// builds a folder's mail list asynchronously after SetCurrentFolder, so a
// loop that never returns to the message loop enumerates only the fraction
// that happened to be ready - which is how a 8,194-mail Sent folder yielded
// 78. And GetSource on a folder of megabyte-sized mails takes long enough
// that holding the UI thread for the whole scan freezes Becky! for minutes.

const int kMaxScan = 400;          // candidates examined, however few qualify
const size_t kPerMailCap = 1500;
// Raised past what ~215 mails produce, so the sample-count setting is not
// silently defeated by the corpus cap before it is reached.
const size_t kTotalCap = 150000;
const size_t kMinUsefulChars = 40; // shorter than this teaches nothing
// The same window the style corpus uses. A narrower one was tried first, on
// the assumption that an attachment is announced early; 700 bytes is about
// 230 Japanese characters, which a salutation and an opening paragraph fill
// on their own, so it cut off the middle of the body - where the announcement
// often actually is.
const size_t kAttachPerMailCap = kPerMailCap;
const size_t kAttachTotalCap = kTotalCap;
// Below this many of either label there is nothing to contrast, and a list
// extracted anyway would be a guess dressed as evidence. Learning simply
// reports that it had too little and leaves the previous list alone.
const int kMinAttachSamples = 5;
const long long kByteBudget = 200LL * 1024 * 1024; // runaway guard only

struct LearnJob
{
	std::string apiKey, model;
	std::string sentFolderId, origFolder, mailboxId;

	std::vector<std::string> ids; // newest-first
	size_t cursor = 0;

	std::string corpus;
	int found = 0;           // mails examined
	int noSource = 0;        // GetSource gave nothing
	int noTextPart = 0;      // no top-level text/plain (HTML-only, etc.)
	int emptyAfterStrip = 0; // only quoted/forwarded material left
	int tooOld = 0;          // outside the age limit
	int collected = 0;
	long long bytesRead = 0;

	int maxMails = 120;
	int cutoffDate = 0;      // yyyymmdd, 0 = no age limit
	int oldestUsed = 0, newestUsed = 0;

	// What the sample actually covers. Counts alone cannot say whether the
	// learning succeeded - only the profile itself can - but when the profile
	// comes out thin they say why: too few new mails, or too few mails per
	// correspondent, rather than the person simply not varying their writing.
	int replyCount = 0, newCount = 0;
	std::map<std::string, int> recipientCounts;

	// Second, independent sample for the forgotten-attachment warning: the
	// same bodies, labelled by whether the mail actually carried a file. Kept
	// apart from `corpus` because it is asked a different question - which
	// wordings predict an attachment - and mixing the labels into the style
	// corpus would only give the style pass something irrelevant to explain.
	std::string attachCorpus;
	int attachYes = 0, attachNo = 0;

	bool listReady = false;
	int lastCount = -1, stableTicks = 0, waitTicks = 0;

	DWORD fetchMs = 0;       // measured, reported in the log
	int fetchCount = 0;

	bool cancelled = false;
};
std::unique_ptr<LearnJob> s_job;

// Mail IDs end in a hex serial ("...\!!!Sent\?1234ABCD") assigned when the
// mail was added to the folder. Ordering by it gives newest-first regardless
// of how the user has the list view sorted.
unsigned long MailSerial(const std::string& mailId)
{
	size_t p = mailId.rfind('?');
	if (p == std::string::npos) return 0;
	return strtoul(mailId.c_str() + p + 1, NULL, 16);
}


// Enough of RFC 2822 to compare dates. Returns yyyymmdd, or 0 if unparsable.
int ParseMailDate(const std::string& v)
{
	static const char* kMonths[] = { "Jan","Feb","Mar","Apr","May","Jun",
	                                 "Jul","Aug","Sep","Oct","Nov","Dec" };
	// Skip an optional "Www, " day-of-week prefix.
	size_t p = v.find(',');
	std::string s = (p != std::string::npos && p <= 4) ? v.substr(p + 1) : v;

	int day = 0, year = 0;
	char mon[8] = {};
	if (sscanf(s.c_str(), " %d %3s %d", &day, mon, &year) != 3) return 0;
	if (day < 1 || day > 31) return 0;
	if (year < 100) year += (year < 50) ? 2000 : 1900; // 2-digit years
	if (year < 1970 || year > 2200) return 0;

	for (int i = 0; i < 12; i++) {
		if (_stricmp(mon, kMonths[i]) == 0) return year * 10000 + (i + 1) * 100 + day;
	}
	return 0;
}

std::wstring FormatDateKey(int key)
{
	wchar_t buf[16];
	swprintf(buf, 16, L"%04d-%02d-%02d", key / 10000, (key / 100) % 100, key % 100);
	return buf;
}

void EndJob()
{
	CloseProgress();
	s_job.reset();
	s_learning = false;
}

void StartAnalysis();

// Waiting for Becky! to finish building the folder's mail list. Enumerating
// IDs is cheap (no GetSource), so poll until the count stops growing.
void LearnTickWaitList()
{
	LearnJob& j = *s_job;
	j.waitTicks++;

	std::vector<std::string> ids;
	char idBuf[512];
	int pos = -1;
	while (true) {
		idBuf[0] = '\0';
		pos = bka.GetNextMail(pos, idBuf, sizeof(idBuf), FALSE);
		if (pos == -1) break;
		if (idBuf[0]) ids.push_back(idBuf);
		if (ids.size() >= 200000) break; // sanity stop
	}
	int count = (int)ids.size();

	char msg[200];
	sprintf(msg, "フォルダの一覧を読み込んでいます...\r\n%d 通を認識", count);
	SetProgress(msg);

	if (count > 0 && count == j.lastCount) {
		j.stableTicks++;
	} else {
		j.stableTicks = 0;
		j.lastCount = count;
	}

	const int kStableTicksNeeded = 4;  // ~1.2s with no change
	const int kMaxWaitTicks = 100;     // ~30s ceiling
	if (j.stableTicks < kStableTicksNeeded && j.waitTicks < kMaxWaitTicks) return;

	std::sort(ids.begin(), ids.end(), [](const std::string& a, const std::string& b) {
		return MailSerial(a) > MailSerial(b);
	});
	j.ids.swap(ids);
	j.listReady = true;
}

// Reading bodies, a few per tick.
void LearnTickFetch()
{
	LearnJob& j = *s_job;
	const int kPerTick = 3;

	for (int n = 0; n < kPerTick; n++) {
		if (j.collected >= j.maxMails || j.cursor >= j.ids.size() ||
		    j.found >= kMaxScan || j.bytesRead >= kByteBudget ||
		    j.corpus.size() > kTotalCap) {
			StartAnalysis();
			return;
		}

		std::string mailId = j.ids[j.cursor++];
		j.found++;

		ULONGLONG t0 = GetTickCount64();
		LPSTR src = bka.GetSource(mailId.c_str());
		j.fetchMs += (DWORD)(GetTickCount64() - t0);
		j.fetchCount++;

		if (!src || !*src) {
			if (src) bka.Free(src);
			j.noSource++;
			continue;
		}
		std::string rawSource = src;
		bka.Free(src); // sources run to megabytes when attachments are present
		j.bytesRead += (long long)rawSource.size();

		// Age limit. Skip rather than stop: the serial is storage order, a
		// proxy for date, and one stray old mail must not end the scan.
		int dateKey = ParseMailDate(GetTopLevelHeader(rawSource, "Date"));
		if (j.cutoffDate && dateKey && dateKey < j.cutoffDate) {
			j.tooOld++;
			continue;
		}

		std::string bodyUtf8 = ExtractPlainTextBodyUtf8(rawSource);
		if (bodyUtf8.empty()) {
			j.noTextPart++;
			continue;
		}

		// Drop other people's words: quoted lines, then anything below an
		// inline forward separator.
		std::string cleaned = StripQuotedLines(StripForwardedTail(bodyUtf8));
		cleaned = TruncateUtf8(cleaned, kPerMailCap);
		if (cleaned.find_first_not_of(" \t\r\n") == std::string::npos ||
		    cleaned.size() < kMinUsefulChars) {
			j.emptyAfterStrip++;
			continue;
		}

		// Label the sample with who it went to and whether it was a reply.
		// Without the recipient the model can see that the register varies but
		// has nothing to key the variation on, and writes "秘書課宛は最敬礼" -
		// true, but unusable at draft time, when all we have is an address.
		// The reply/new distinction matters for openings: a new mail has to
		// introduce itself, a reply answers something already said.
		// Only the first To address. Labelling a mail with every recipient
		// made one message written for one person count as evidence for all
		// of them, so a rule for B absorbed prose that was aimed at C. The
		// primary addressee is conventionally listed first.
		std::string toHeader = GetTopLevelHeader(rawSource, "To");
		std::string toAddrs = ExtractAddresses(toHeader, 1);
		bool isReply = !GetTopLevelHeader(rawSource, "In-Reply-To").empty() ||
		               !GetTopLevelHeader(rawSource, "References").empty();
		if (isReply) j.replyCount++; else j.newCount++;

		std::string primary = ExtractAddresses(toHeader, 1);
		if (!primary.empty()) j.recipientCounts[primary]++;

		const char* kind = isReply ? "返信" : "新規";
		j.corpus += toAddrs.empty()
			? ("--- Email (" + std::string(kind) + ") ---\n")
			: ("--- Email (to: " + toAddrs + ", " + kind + ") ---\n");
		j.corpus += cleaned;
		j.corpus += "\n";
		j.collected++;

		// Attachment evidence. Shorter per mail than the style sample: an
		// announcement of an attachment is made where the attachment is
		// introduced, which is never buried at the end of a long mail, and
		// both labels have to fit in one request.
		if (j.attachCorpus.size() < kAttachTotalCap) {
			bool hasAttachment = SourceHasAttachment(rawSource);
			if (hasAttachment) j.attachYes++; else j.attachNo++;
			j.attachCorpus += hasAttachment ? "--- [ATTACHED] ---\n" : "--- [NO FILE] ---\n";
			j.attachCorpus += TruncateUtf8(cleaned, kAttachPerMailCap);
			j.attachCorpus += "\n";
		}

		if (dateKey) {
			if (!j.newestUsed || dateKey > j.newestUsed) j.newestUsed = dateKey;
			if (!j.oldestUsed || dateKey < j.oldestUsed) j.oldestUsed = dateKey;
		}
	}

	char msg[200];
	sprintf(msg, "送信済みメールを読み込み中\r\n%d/%d 通を走査、%d/%d 通を収集",
		j.found, (int)j.ids.size(), j.collected, j.maxMails);
	SetProgress(msg);
}

void LearnTick()
{
	if (!s_job) return;
	if (s_job->cancelled) {
		if (!s_job->origFolder.empty()) bka.SetCurrentFolder(s_job->origFolder.c_str());
		EndJob();
		return;
	}
	if (!s_job->listReady) LearnTickWaitList();
	else LearnTickFetch();
}

void LearnCancel()
{
	if (s_job) s_job->cancelled = true;
}

} // namespace

void LearnStyleForFolder(HWND hWnd, const std::string& sentFolderId)
{
	if (s_learning) {
		MessageBoxW(hWnd, L"送信済みメールの学習を実行中です。完了までお待ちください。",
			L"Becky! Claude Plugin", MB_OK | MB_ICONINFORMATION);
		return;
	}

	std::string apiKey, model;
	if (!RequireApiKey(apiKey, model)) return;

	if (sentFolderId.empty()) {
		MessageBoxW(hWnd, L"学習フォルダが指定されていません。",
			L"Becky! Claude Plugin", MB_OK | MB_ICONWARNING);
		return;
	}

	PluginSettings settings;
	LoadSettings(settings);

	s_learning = true;
	s_job.reset(new LearnJob());
	LearnJob& j = *s_job;
	j.apiKey = apiKey;
	j.model = model;
	j.sentFolderId = sentFolderId;
	j.mailboxId = MailboxIdFromFolderId(sentFolderId);
	j.maxMails = (settings.learnMaxMails > 0) ? settings.learnMaxMails : 120;

	if (settings.learnMaxMonths > 0) {
		SYSTEMTIME st;
		GetLocalTime(&st);
		// Month arithmetic on a 0-based month index. The day is carried over
		// as-is: a 31st landing in a 30-day month makes an impossible date,
		// but this is only ever compared numerically as a boundary.
		int months = st.wYear * 12 + (st.wMonth - 1) - settings.learnMaxMonths;
		j.cutoffDate = (months / 12) * 10000 + (months % 12 + 1) * 100 + st.wDay;
	}

	OpenProgress();
	SetProgress("フォルダの一覧を読み込んでいます...");

	LPCTSTR lpOrigFolder = bka.GetCurrentFolder();
	j.origFolder = lpOrigFolder ? lpOrigFolder : "";

	// GetNextMail enumerates the folder that is open, so switch to the
	// registered one. No SetCurrentMail anywhere: reading by ID via GetSource
	// keeps us out of Becky!'s asynchronous "current mail" machinery, which
	// is what made an earlier attempt return nothing (and crash when pumped).
	bka.SetCurrentFolder(sentFolderId.c_str());

	// From here the job runs on the progress window's timer, so Becky! gets
	// its message loop back between steps and can finish loading the folder.
	if (s_progressDlg) SetTimer(s_progressDlg, kLearnTimerId, 300, NULL);
}

namespace {

// What the completion message reports. Held in a shared_ptr because the run
// now spans two chained requests and the second one adds to it.
struct LearnSummary
{
	int listSize = 0, found = 0, collected = 0, tooOld = 0;
	int oldestUsed = 0, newestUsed = 0, avgFetchMs = 0;
	int replyCount = 0, newCount = 0;
	int recipientCount = 0, wellCovered = 0;
	int attachYes = 0, attachNo = 0;
	size_t attachCorpusBytes = 0;
	std::wstring attachLine; // one line on what the attachment pass did

	std::string mailboxId;
	std::string rawPhraseAnswer; // exactly what the model returned, unfiltered
	int cleanedPhrases = 0;      // how many survived CleanAttachPhrases
};

// A learning run reports itself in a dialog that is gone the moment it is
// dismissed, and by then the interesting part - why a pass produced nothing -
// is unrecoverable. So every run also appends a block here. The raw model
// answer is included because the two failures it distinguishes need opposite
// fixes: an empty answer means the extraction rule was too strict, while a
// non-empty one that cleaned down to nothing means our own filter rejected it.
void AppendLearnLog(const LearnSummary& s)
{
	std::string path = GetPluginDataDir() + "\\learn.log";
	FILE* fp = fopen(path.c_str(), "a");
	if (!fp) return;

	SYSTEMTIME st;
	GetLocalTime(&st);
	fprintf(fp, "=== %04d-%02d-%02d %02d:%02d:%02d  mailbox=%s\n",
		st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
		s.mailboxId.c_str());
	fprintf(fp, "  scan   : list=%d found=%d collected=%d reply=%d new=%d recipients=%d(>=3: %d)\n",
		s.listSize, s.found, s.collected, s.replyCount, s.newCount,
		s.recipientCount, s.wellCovered);
	fprintf(fp, "  attach : yes=%d no=%d corpus=%u bytes\n",
		s.attachYes, s.attachNo, (unsigned)s.attachCorpusBytes);
	fprintf(fp, "  result : %s\n", WideToUtf8(s.attachLine).c_str());

	if (!s.rawPhraseAnswer.empty()) {
		// Capped: a model that ignored the format could return anything, and
		// this is a log, not a transcript.
		std::string raw = TruncateUtf8(s.rawPhraseAnswer, 2000);
		fprintf(fp, "  cleaned: %d phrase(s) kept from %u bytes of answer\n",
			s.cleanedPhrases, (unsigned)s.rawPhraseAnswer.size());
		fprintf(fp, "  --- raw answer ---\n%s\n  --- end ---\n", raw.c_str());
	}

	fclose(fp);
}

void ShowLearnSummary(const LearnSummary& s)
{
	AppendLearnLog(s);

	// The date range makes the sample verifiable: the scan orders by storage
	// serial, which is only a proxy for date, so showing what was actually
	// learned lets a bad assumption be spotted.
	std::wstring range;
	if (s.oldestUsed && s.newestUsed) {
		range = L"対象期間: " + FormatDateKey(s.oldestUsed) +
		        L" 〜 " + FormatDateKey(s.newestUsed) + L"\r\n";
	}

	wchar_t buf[1024];
	swprintf(buf, 1024,
		L"送信済みメールの学習が完了しました。\r\n\r\n"
		L"%s"
		L"フォルダ内 %d通 / 走査 %d通 / 学習に使用 %d通%s\r\n"
		L"内訳: 返信 %d通 / 新規 %d通\r\n"
		L"宛先 %d件 (うち3通以上ある宛先 %d件)\r\n"
		L"1通あたりの読み込み: 約%dミリ秒\r\n"
		L"%s",
		range.c_str(), s.listSize, s.found, s.collected,
		s.tooOld ? L" (期間外を除外)" : L"",
		s.replyCount, s.newCount, s.recipientCount, s.wellCovered, s.avgFetchMs,
		s.attachLine.c_str());
	MessageBoxW(NULL, buf, L"Becky! Claude Plugin", MB_OK | MB_ICONINFORMATION);
}

// Turns the phrase pass's answer into the file's contents: one phrase per
// line, nothing else. The model is told exactly this format, but the list is
// matched literally against outgoing mail, so a stray bullet or a sentence
// that slipped through would be a phrase that can never match - or, worse,
// one so short that it matches everything.
std::string CleanAttachPhrases(const std::string& answer, int& outCount)
{
	// In UTF-8 bytes. Four Japanese characters is the floor. Two was too low:
	// asked for the shortest form that carries the meaning, a model answers
	// with a stem - 「添付し」 - which then matches 「添付し忘れ」 and
	// 「添付していません」 as readily as 「添付します」. The ceiling rejects
	// whole sentences, which carry mail-specific wording and never recur
	// verbatim.
	const size_t kMinBytes = 12, kMaxBytes = 60;
	const int kMaxPhrases = 20;

	// An announcement of an attachment says so. Phrases that do not -
	// 「資料をお送りします」, 「送ります。」 - were offered and are exactly the
	// ones that also fit a mail sending nothing, by post or later or as a
	// link. The extraction is told to exclude them, but that instruction is
	// the model's assertion about 200 mails rather than something it can
	// really verify, so the requirement is enforced here as well. It costs
	// the wordings that announce a file without naming it; that is the right
	// side of the trade when a false warning is the expensive failure, and
	// such a phrase can still be added to the file by hand.
	const char* kMarkers[] = { "添付", "別添" };

	std::vector<std::string> kept;

	size_t pos = 0;
	while (pos <= answer.size() && (int)kept.size() < kMaxPhrases) {
		size_t eol = answer.find('\n', pos);
		size_t end = (eol == std::string::npos) ? answer.size() : eol;
		std::string line = answer.substr(pos, end - pos);
		if (eol == std::string::npos) pos = answer.size() + 1; else pos = eol + 1;

		// Strip list markers and quoting the model may have added anyway.
		// ASCII only: find_first_not_of works on bytes, and putting a
		// multi-byte character in the set would let it eat the lead byte of a
		// perfectly good Japanese phrase and leave a broken tail behind.
		size_t first = line.find_first_not_of(" \t\r-*0123456789.)");
		if (first == std::string::npos) continue;
		line.erase(0, first);
		while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) {
			line.pop_back();
		}
		auto stripPair = [&line](const char* open, const char* close) {
			size_t o = strlen(open), c = strlen(close);
			if (line.size() > o + c && line.compare(0, o, open) == 0 &&
			    line.compare(line.size() - c, c, close) == 0) {
				line = line.substr(o, line.size() - o - c);
			}
		};
		stripPair("\"", "\"");
		stripPair("「", "」");

		if (line.size() < kMinBytes || line.size() > kMaxBytes) continue;
		// A phrase with a digit or an address in it came from one specific
		// mail and will never match another.
		if (line.find_first_of("0123456789@") != std::string::npos) continue;

		bool marked = false;
		for (const char* m : kMarkers) {
			if (line.find(m) != std::string::npos) { marked = true; break; }
		}
		if (!marked) continue;

		bool duplicate = false;
		for (const std::string& seen : kept) {
			if (seen == line) { duplicate = true; break; }
		}
		if (duplicate) continue;

		kept.push_back(line);
	}

	// Matching is by substring, so a phrase that contains another phrase on
	// the list can never fire: the shorter one always matches first. Such a
	// line is not merely useless - it makes the file misleading to read and
	// to edit by hand, which is the one thing this format is for.
	//
	// The prompt now states the rule, so this is the safety net rather than
	// the mechanism: a model can ignore an instruction, and a person editing
	// the file can add a long line without thinking about it.
	//
	// Shortest first, because the shorter form is the more general one and
	// has to be the one that survives. Stable, so equal lengths keep the
	// order the model gave them.
	std::stable_sort(kept.begin(), kept.end(),
		[](const std::string& a, const std::string& b) { return a.size() < b.size(); });

	std::string out;
	std::vector<std::string> final;
	for (const std::string& phrase : kept) {
		bool subsumed = false;
		for (const std::string& shorter : final) {
			if (phrase.find(shorter) != std::string::npos) { subsumed = true; break; }
		}
		if (subsumed) continue;
		final.push_back(phrase);
		out += phrase;
		out += "\r\n";
	}

	outCount = (int)final.size();
	return out;
}

// The second request of a learning run. Ends the job either way: a failure
// here leaves the previous phrase list (and the style profile just saved)
// untouched, which is the fail-open behaviour the whole feature is built on.
void StartAttachPhrasePass(const std::string& mailboxId, const std::string& apiKey,
                            const std::string& model, const std::string& attachCorpus,
                            std::shared_ptr<LearnSummary> sum)
{
	SetProgress("添付ファイルに関する表現を抽出しています...");

	std::string systemPrompt =
		"You are given email bodies written by one person, each labelled "
		"[ATTACHED] (that mail carried a file attachment) or [NO FILE] (it did "
		"not). Quoted replies have been removed, so every line is this person's "
		"own writing.\n\n"
		"Find the phrases this person uses to announce that they are attaching a "
		"file. Output ONE PHRASE PER LINE and nothing else - no numbering, no "
		"bullets, no quotation marks, no explanation, no heading, no blank "
		"lines.\n\n"
		"A phrase qualifies only if BOTH of these hold:\n"
		"  - it appears in at least two [ATTACHED] mails, and\n"
		"  - it appears in NO [NO FILE] mail.\n"
		"The second test is the important one. A phrase that also turns up in "
		"[NO FILE] mails is disqualified however common it is among the "
		"[ATTACHED] ones.\n\n"
		"HOW THE LIST IS USED, which decides what belongs on it: each line is "
		"searched for as a SUBSTRING of the new mail's text. A line fires if it "
		"occurs anywhere in the text.\n\n"
		"What that means for your list:\n"
		"  - Never give a line that CONTAINS another line of your own list. If "
		"you list 「添付の通り」, then 「添付の通りお送りします」 can never fire - "
		"the shorter line already matched. Listing both wastes a line. List "
		"each wording once and move on to a genuinely different one.\n"
		"  - But do NOT shorten a phrase to make it general. Give it as it "
		"actually appears, and long enough that seeing those exact characters "
		"in a mail is on its own good evidence that a file is attached. A stem "
		"or a fragment - 「添付し」, 「添付の」 - is not: it matches "
		"「添付し忘れました」 and 「添付の件ですが」 just as well. Four Japanese "
		"characters is about the shortest that can carry the meaning.\n"
		"  - Every line must actually say that something is attached. "
		"「資料をお送りします」 and 「送ります」 do not - they fit a mail that "
		"attaches nothing and sends the material by post, by link, or later.\n"
		"  - A phrase about an attachment that is MISSING is not an "
		"announcement. 「添付し忘れました」, 「添付がありませんでした」 and the "
		"like belong to mails apologising for the opposite situation; never "
		"list them.\n"
		"  - Nothing specific to one mail: no file names, dates, numbers, "
		"project names or people's names. Those occur once and never again.\n"
		"  - Copy each phrase exactly as it appears, same script and same "
		"inflection; do not normalise, generalise or translate it.\n"
		"  - Never invent a phrase you did not actually see.\n"
		"  - At most 20 lines.\n\n"
		"Your lines should read as a set of distinct ways this person announces "
		"an attachment - not as one wording plus its variations, and not as "
		"stems of one another.\n\n"
		"If no phrase passes both tests, output nothing at all. An empty answer "
		"is a correct answer here, and a far better one than a loose phrase: "
		"every phrase on this list that can appear without an attachment turns "
		"into a false warning on a mail that was perfectly fine.";

	// The answer is 20 short lines - a couple of hundred tokens - but getting
	// to it means cross-referencing every phrase against both sets of mails,
	// which is real work and which the model does inside its token budget. At
	// 1536 with no cap it spent the lot on reasoning and returned nothing at
	// all. Capping the reasoning leaves 2192 tokens that belong to the list
	// and cannot be spent on anything else.
	AsyncRunner_CallClaude(apiKey, model, systemPrompt, attachCorpus, 8192,
		[mailboxId, sum](bool success, const std::string& text, const std::string& error) {
			EndJob();

			// The sample counts go on every outcome, not just the successful
			// one: without them a failure says nothing about whether the
			// mailbox had anything to learn from in the first place.
			wchar_t sample[80];
			swprintf(sample, 80, L" (添付あり %d通 / なし %d通)",
				sum->attachYes, sum->attachNo);

			if (!success) {
				sum->attachLine = L"添付忘れ警告: 表現の抽出に失敗" + std::wstring(sample) +
				                  L"\r\n" + Utf8ToWide(error);
			} else {
				int count = 0;
				std::string phrases = CleanAttachPhrases(text, count);
				sum->rawPhraseAnswer = text;
				sum->cleanedPhrases = count;
				if (count == 0) {
					sum->attachLine = L"添付忘れ警告: 該当する表現が見つかりませんでした" + std::wstring(sample);
				} else if (!SaveAttachPhrases(mailboxId, phrases)) {
					sum->attachLine = L"添付忘れ警告: 表現リストの保存に失敗しました";
				} else {
					wchar_t line[80];
					swprintf(line, 80, L"添付忘れ警告: %d件の表現を学習", count);
					sum->attachLine = std::wstring(line) + sample;
				}
			}
			ShowLearnSummary(*sum);
		},
		6000);
}

void StartAnalysis()
{
	LearnJob& j = *s_job;

	if (s_progressDlg) KillTimer(s_progressDlg, kLearnTimerId);
	if (!j.origFolder.empty()) bka.SetCurrentFolder(j.origFolder.c_str());

	if (j.collected == 0) {
		wchar_t buf[400];
		swprintf(buf, 400,
			L"学習フォルダから、学習に使えるメール本文が見つかりませんでした。\r\n"
			L"(一覧=%d, 走査=%d, ソース取得失敗=%d, 本文パート無し=%d, 引用のみ=%d, 期間外=%d)",
			(int)j.ids.size(), j.found, j.noSource, j.noTextPart, j.emptyAfterStrip, j.tooOld);
		EndJob();
		MessageBoxW(NULL, buf, L"Becky! Claude Plugin", MB_OK | MB_ICONWARNING);
		return;
	}

	std::string systemPrompt =
		"You will see several email bodies, all written by the same person "
		"(quoted replies removed), each labelled with the address it was sent "
		"to. Produce a compact Japanese style guide describing how this person "
		"writes: formality/keigo level, typical greetings and closings, "
		"first-person pronoun, sentence length and rhythm, common phrases, and "
		"how they phrase requests/apologies/declines. Keep it under 1000 "
		"Japanese characters, written as a guide another writer could follow, "
		"not a report about the analysis.\n\n"
		"Where the writing changes by recipient, state the rule in terms of "
		"the actual addresses and domains - never in terms of job titles or "
		"relationships. This guide is applied with nothing but an email "
		"address in hand, so 「秘書課宛は最敬礼」 cannot be acted on, while "
		"「a@example.jp, b@example.jp 宛は最敬礼」 or 「@example.ac.jp 宛は"
		"砕けた丁寧体」 can. Name individual addresses where recipients within "
		"one domain differ; group by domain where the whole domain is treated "
		"alike. Say nothing about a recipient you have only one or two samples "
		"of - an unsupported rule is worse than none.\n\n"
		"One person does not always get one register. The same recipient may be "
		"written to differently depending on the matter at hand - a project, a "
		"formal request, a casual aside. Where a recipient's samples disagree "
		"with each other, SAY SO and describe what varies, instead of averaging "
		"them or picking whichever is more numerous. A confident rule built "
		"from one context will be applied in another where it does not belong; "
		"'この宛先は案件により異なる' is more useful than a wrong certainty.\n\n"
		"Each sample is also marked 返信 or 新規. Cover both: a new mail has to "
		"introduce itself and state its business unopposed, while a reply "
		"answers something already said, so the openings and closings differ. "
		"Say explicitly how each begins and ends, with the actual phrases used. "
		"If one of the two is too thinly represented to characterise, say so "
		"rather than inventing a pattern for it.\n\n"
		"Ignore signature blocks entirely - name, organisation, address, phone, "
		"email and URL sign-offs at the end of a mail. The mail client appends "
		"those automatically, so they are not part of how this person writes. "
		"Say nothing about them in the guide. Closing greetings in the body "
		"itself are part of the style and should be covered.";

	{
		char msg[200];
		sprintf(msg, "%d通をClaudeで分析しています...", j.collected);
		SetProgress(msg);
	}

	// Copy everything the completion needs; the job is torn down once the
	// request is in flight.
	std::string mailboxId = j.mailboxId;
	std::string apiKey = j.apiKey, model = j.model, corpus = j.corpus;
	std::string attachCorpus = j.attachCorpus;

	auto sum = std::make_shared<LearnSummary>();
	sum->collected = j.collected;
	sum->found = j.found;
	sum->listSize = (int)j.ids.size();
	sum->tooOld = j.tooOld;
	sum->oldestUsed = j.oldestUsed;
	sum->newestUsed = j.newestUsed;
	sum->avgFetchMs = j.fetchCount ? (int)(j.fetchMs / j.fetchCount) : 0;
	sum->replyCount = j.replyCount;
	sum->newCount = j.newCount;
	sum->recipientCount = (int)j.recipientCounts.size();
	sum->attachYes = j.attachYes;
	sum->attachNo = j.attachNo;
	sum->attachCorpusBytes = j.attachCorpus.size();
	sum->mailboxId = j.mailboxId;
	for (const auto& kv : j.recipientCounts) {
		if (kv.second >= 3) sum->wellCovered++; // enough samples to base a rule on
	}

	int collected = j.collected;

	// The prompt asks for under 1000 Japanese characters and the cards
	// actually produced run to about 1700, so 2048 sat right on the edge and
	// eventually went over; 4096 then failed too, which says the output is not
	// only the card text. Given generously here: the card is written once per
	// learning run, so headroom costs nothing, and a truncated card stays an
	// error - it would be missing whichever section came last.
	AsyncRunner_CallClaude(apiKey, model, systemPrompt, corpus, 8192,
		[mailboxId, model, apiKey, collected, attachCorpus, sum]
		(bool success, const std::string& text, const std::string& error) {
			// The settings dialog may already be gone, so parent to NULL.
			if (!success) {
				EndJob();
				MessageBoxW(NULL, (L"エラー: " + Utf8ToWide(error)).c_str(),
					L"Becky! Claude Plugin", MB_OK | MB_ICONERROR);
				return;
			}
			if (!SaveStyleProfile(mailboxId, text)) {
				EndJob();
				MessageBoxW(NULL, L"学習データの保存に失敗しました。",
					L"Becky! Claude Plugin", MB_OK | MB_ICONERROR);
				return;
			}

			SYSTEMTIME st;
			GetLocalTime(&st);
			char stamp[32];
			sprintf(stamp, "%04d-%02d-%02d %02d:%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);

			std::vector<SentFolderInfo> folders = LoadSentFolders();
			for (SentFolderInfo& f : folders) {
				if (MailboxIdFromFolderId(f.folderId) == mailboxId) {
					f.learnedAt = stamp;
					f.sampleCount = collected;
					f.learnedModel = model;
				}
			}
			SaveSentFolders(folders);

			// Second pass, on the same sample: which wordings announce an
			// attachment. Deliberately a separate request rather than one
			// prompt asking for both - the two answers have different shapes
			// (prose vs. a literal match list) and different failure modes,
			// and a failure here must not cost the style profile just saved.
			//
			// Runs whether or not the send-time warning is switched on. The
			// phrase list can only come from here, and learning is a rare,
			// deliberate act: making its output depend on an unrelated toggle
			// would mean switching the warning back on silently does nothing
			// until the folder is learned again. The cost of getting that
			// wrong is worse than one extra request per learning run.
			if (sum->attachYes < kMinAttachSamples || sum->attachNo < kMinAttachSamples) {
				wchar_t line[160];
				swprintf(line, 160,
					L"添付忘れ警告: 標本が不足のため見送り (添付あり %d通 / なし %d通)",
					sum->attachYes, sum->attachNo);
				sum->attachLine = line;
			} else {
				StartAttachPhrasePass(mailboxId, apiKey, model, attachCorpus, sum);
				return; // that pass ends the job and shows the summary
			}

			EndJob();
			ShowLearnSummary(*sum);
		});
}

} // namespace

bool CurrentFolderIsLearnTarget()
{
	LPCTSTR lpFolderID = bka.GetCurrentFolder();
	if (!lpFolderID || !*lpFolderID) return false;
	std::string folderId = lpFolderID;
	return folderId == EffectiveSentFolderId(MailboxIdFromFolderId(folderId));
}

void CALLBACK OnCmdRegisterSentFolder(HWND hWnd, LPARAM /*lParam*/)
{
	LPCTSTR lpFolderID = bka.GetCurrentFolder();
	if (!lpFolderID || !*lpFolderID) {
		MessageBoxW(hWnd, L"フォルダを取得できませんでした。",
			L"Becky! Claude Plugin", MB_OK | MB_ICONWARNING);
		return;
	}
	std::string folderId = lpFolderID;
	std::string mailboxId = MailboxIdFromFolderId(folderId);

	LPCSTR lpName = bka.GetFolderDisplayName(folderId.c_str());
	std::wstring folderName = Utf8ToWide(AnsiToUtf8(
		std::string((lpName && *lpName) ? lpName : folderId.c_str())));

	// Pointing at something other than Becky!'s own Sent folder is a valid
	// thing to want - some people file sent mail themselves, or want the
	// profile learned from one narrow folder. It is also what a misclick
	// looks like, so confirm. Phrased as the action, not as a rejection.
	if (folderId != DerivedSentFolderId(mailboxId)) {
		std::wstring msg =
			L"このフォルダを学習フォルダとして指定しますか?\r\n\r\n    "
			+ folderName +
			L"\r\n\r\nBecky! の送信済みフォルダではありませんが、送信メールを"
			L"このフォルダに分けている場合は、このまま指定してください。";
		if (MessageBoxW(hWnd, msg.c_str(), L"Becky! Claude Plugin",
				MB_OKCANCEL | MB_ICONQUESTION) != IDOK) {
			return;
		}
	}

	// One Sent folder per mailbox: registering a second one in the same
	// mailbox replaces the first rather than adding a duplicate.
	std::vector<SentFolderInfo> folders = LoadSentFolders();
	bool replaced = false;
	for (SentFolderInfo& f : folders) {
		if (MailboxIdFromFolderId(f.folderId) == mailboxId) {
			if (f.folderId != folderId) {
				f.folderId = folderId;
				f.learnedAt.clear(); // a different folder means a stale profile
				f.sampleCount = 0;
			}
			replaced = true;
			break;
		}
	}
	if (!replaced) {
		SentFolderInfo info;
		info.folderId = folderId;
		folders.push_back(info);
	}
	SaveSentFolders(folders);

	std::wstring msg = L"次のフォルダを学習フォルダとして指定しました。\r\n\r\n    "
		+ folderName
		+ L"\r\n\r\n続けて、同じ右クリックメニューの"
		  L"「Claude で送信済みメールを学習」を実行してください。";
	MessageBoxW(hWnd, msg.c_str(), L"Becky! Claude Plugin", MB_OK | MB_ICONINFORMATION);
}

void CALLBACK OnCmdLearnStyle(HWND hWnd, LPARAM /*lParam*/)
{
	LPCTSTR lpFolderID = bka.GetCurrentFolder();
	std::string folderId = lpFolderID ? lpFolderID : "";
	if (folderId.empty()) {
		MessageBoxW(hWnd, L"フォルダを取得できませんでした。",
			L"Becky! Claude Plugin", MB_OK | MB_ICONWARNING);
		return;
	}

	std::string mailboxId = MailboxIdFromFolderId(folderId);
	if (folderId != EffectiveSentFolderId(mailboxId)) {
		MessageBoxW(hWnd,
			L"このフォルダは、このメールボックスの学習フォルダではありません。\r\n\r\n"
			L"このフォルダから学習させたい場合は、先に"
			L"「Claude で学習フォルダとして指定」を実行してください。",
			L"Becky! Claude Plugin", MB_OK | MB_ICONWARNING);
		return;
	}

	// Learning the auto-derived folder records it, so the settings list has a
	// row to show the learned state on and to delete the profile from.
	std::vector<SentFolderInfo> folders = LoadSentFolders();
	bool known = false;
	for (const SentFolderInfo& f : folders) {
		if (f.folderId == folderId) { known = true; break; }
	}
	if (!known) {
		SentFolderInfo info;
		info.folderId = folderId;
		folders.push_back(info);
		SaveSentFolders(folders);
	}

	// Not inline: the scan calls SetCurrentFolder and runs for a while, and
	// this callback sits inside Becky!'s own tree context-menu command
	// handling. Let that unwind first and start from a plain message dispatch.
	AsyncRunner_RunLater([folderId]() {
		LearnStyleForFolder(NULL, folderId);
	});
}

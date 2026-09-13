#include <windows.h>
#include <string>
#include <vector>
#include <cstdio>
#include "MailActions.h"
#include "BeckyApi.h"
#include "Settings.h"
#include "Encoding.h"
#include "Cache.h"
#include "ResultDialog.h"
#include "PluginUtil.h"

extern CBeckyAPI bka;

namespace {

bool LooksLikeHtml(const std::string& mimeType, const std::string& body)
{
	if (ToLowerAscii(mimeType).find("html") != std::string::npos) return true;
	std::string head = ToLowerAscii(body.substr(0, 2000));
	return head.find("<html") != std::string::npos ||
	       head.find("<!doctype html") != std::string::npos ||
	       head.find("<body") != std::string::npos;
}

void AppendEntity(std::string& out, const std::string& name)
{
	if (name == "nbsp") { out += ' '; return; }
	if (name == "amp") { out += '&'; return; }
	if (name == "lt") { out += '<'; return; }
	if (name == "gt") { out += '>'; return; }
	if (name == "quot") { out += '"'; return; }
	if (name == "apos" || name == "#39") { out += '\''; return; }
	if (name.size() > 1 && name[0] == '#') {
		long cp = 0;
		try {
			cp = (name[1] == 'x' || name[1] == 'X')
				? std::stol(name.substr(2), nullptr, 16)
				: std::stol(name.substr(1), nullptr, 10);
		} catch (...) { out += '&'; out += name; out += ';'; return; }
		if (cp > 0 && cp <= 0x10FFFF) {
			wchar_t w[2] = { (wchar_t)cp, 0 };
			out += WideToUtf8(w);
			return;
		}
	}
	// Unknown entity: leave it as-is.
	out += '&'; out += name; out += ';';
}

// Rough HTML-to-text: drops <script>/<style>/<head> wholesale, turns block
// tags into newlines, strips remaining tags, decodes common entities.
// Good enough to give Claude the readable content of an HTML-only mail.
std::string HtmlToPlainText(const std::string& html)
{
	std::string lower = ToLowerAscii(html);
	std::string out;
	out.reserve(html.size() / 2);

	size_t i = 0;
	while (i < html.size()) {
		if (html[i] == '<') {
			// Skip whole <script>/<style>/<head> ... </...> regions.
			const char* skipTags[] = { "script", "style", "head" };
			bool skipped = false;
			for (const char* tag : skipTags) {
				std::string open = std::string("<") + tag;
				if (lower.compare(i, open.size(), open) == 0) {
					std::string close = std::string("</") + tag + ">";
					size_t end = lower.find(close, i);
					i = (end == std::string::npos) ? html.size() : end + close.size();
					skipped = true;
					break;
				}
			}
			if (skipped) continue;

			size_t gt = html.find('>', i);
			if (gt == std::string::npos) break;
			std::string tag = ToLowerAscii(html.substr(i + 1, gt - i - 1));
			if (tag.rfind("br", 0) == 0 || tag.rfind("/p", 0) == 0 || tag.rfind("/div", 0) == 0 ||
			    tag.rfind("/tr", 0) == 0 || tag.rfind("/li", 0) == 0 || tag.rfind("/h", 0) == 0 ||
			    tag.rfind("p", 0) == 0 || tag.rfind("li", 0) == 0) {
				out += '\n';
			}
			i = gt + 1;
			continue;
		}
		if (html[i] == '&') {
			size_t semi = html.find(';', i);
			if (semi != std::string::npos && semi - i <= 10) {
				AppendEntity(out, html.substr(i + 1, semi - i - 1));
				i = semi + 1;
				continue;
			}
		}
		out += html[i];
		i++;
	}

	// Collapse whitespace: trim each line, drop runs of blank lines.
	std::string cleaned;
	size_t start = 0;
	int blankRun = 0;
	while (start <= out.size()) {
		size_t nl = out.find('\n', start);
		size_t end = (nl == std::string::npos) ? out.size() : nl;
		std::string line = out.substr(start, end - start);
		size_t a = line.find_first_not_of(" \t\r");
		size_t b = line.find_last_not_of(" \t\r");
		line = (a == std::string::npos) ? "" : line.substr(a, b - a + 1);
		if (line.empty()) {
			if (++blankRun <= 1) cleaned += '\n';
		} else {
			blankRun = 0;
			cleaned += line;
			cleaned += '\n';
		}
		if (nl == std::string::npos) break;
		start = nl + 1;
	}
	return cleaned;
}
// The model has no clock, so "3 days ago" / "the deadline has passed" style
// judgements need today's date supplied alongside the mail's Date header.
std::string CurrentDateTimeString()
{
	SYSTEMTIME st;
	GetLocalTime(&st);
	char buf[64];
	sprintf(buf, "%04d-%02d-%02d %02d:%02d (local time)",
		st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
	return buf;
}

// Who the mail is from/to, plus subject and date - the participant facts that
// aren't recoverable from the body alone.
std::string BuildHeaderBlockUtf8()
{
	static const char* kNames[] = { "From", "To", "Cc", "Subject", "Date" };
	std::string out;
	for (const char* name : kNames) {
		std::string value = GetHeaderUtf8(name);
		if (value.empty()) continue;
		out += name;
		out += ": ";
		out += value;
		out += "\n";
	}
	return out;
}

// Gets the currently viewed mail's text and shows a result dialog (see
// ShowResultDialog) that sends it to Claude with the given instruction, or
// shows a cached result from a previous call. `cacheFeature` distinguishes
// summarize/translate cache entries for the same mail. `includeHeaders` adds
// the From/To/Cc/Subject/Date block above the body. The mail itself is never
// modified.
void RunMsgViewToResultDialog(HWND hWnd, const std::wstring& title, const char* cacheFeature,
                               const std::string& systemPrompt, int maxTokens,
                               bool includeHeaders = false, bool sectioned = false)
{
	char mimeType[64] = {};
	LPSTR lpBody = bka.GetText(mimeType, sizeof(mimeType));
	if (!lpBody || !*lpBody) {
		MessageBoxW(hWnd, L"メール本文を取得できませんでした。", L"Becky! Claude Plugin", MB_OK | MB_ICONWARNING);
		return;
	}

	LPCTSTR lpMailId = bka.GetCurrentMail();
	std::string mailId = lpMailId ? lpMailId : "";

	PluginSettings settings;
	LoadSettings(settings);
	int cacheMinutes = (std::string(cacheFeature) == "summarize")
		? settings.summarizeCacheMinutes
		: settings.translateCacheMinutes;

	// The model comes from settings rather than from RequireApiKey below,
	// because the variant is needed before the cache is consulted - and a
	// cached answer should still display when no key is configured.
	std::string variant = CacheVariant(settings.model, systemPrompt, maxTokens);

	ResultDlgRequest req;
	req.title = title;
	req.ownerHwnd = hWnd;
	req.sectioned = sectioned;
	req.cacheFeature = cacheFeature;
	req.cacheVariant = variant;
	req.cacheMailId = mailId;
	req.cacheMinutes = cacheMinutes;

	std::string cachedText;
	if (LoadCachedResult(cacheFeature, variant, mailId, cacheMinutes, cachedText)) {
		req.hasCachedResult = true;
		req.cachedResultUtf8 = cachedText;
		ShowResultDialog(req);
		return;
	}

	std::string apiKey, model;
	if (!RequireApiKey(apiKey, model)) return;

	char charsetName[64] = {};
	int codepage = bka.GetCharSet(NULL, charsetName, sizeof(charsetName));

	// Bound raw processing, but do it before HTML-stripping / truncation so
	// an HTML mail's real text (often after lots of CSS) still gets through.
	std::string body = lpBody;
	if (body.size() > 200000) {
		body = body.substr(0, 200000);
	}
	std::string content = AnsiToUtf8(body, codepage > 0 ? (unsigned int)codepage : 0);
	if (LooksLikeHtml(mimeType, content)) {
		content = HtmlToPlainText(content);
	}
	content = TruncateUtf8(content, kMaxBodyChars);

	if (includeHeaders) {
		content = "--- Current date/time ---\n" + CurrentDateTimeString() + "\n\n"
		          "--- Email headers ---\n" + BuildHeaderBlockUtf8() +
		          "\n--- Email body ---\n" + content;
	}

	req.apiKey = apiKey;
	req.model = model;
	req.systemPrompt = systemPrompt;
	req.userContent = content;
	req.maxTokens = maxTokens;
	ShowResultDialog(req);
}

} // namespace

void CALLBACK OnCmdSummarize(HWND hWnd, LPARAM /*lParam*/)
{
	std::string systemPrompt =
		"You summarize a received email for its recipient. Write in Japanese.\n"
		"You are given the current date/time, the email's headers, then its "
		"body. The reader of your summary is the recipient of this mail (one of "
		"the To/Cc addresses); refer to them as あなた.\n"
		"\n"
		"Output exactly these three sections, in this order, using these "
		"headings verbatim and nothing else:\n"
		"\n"
		"【メール送受信者】\n"
		"This section is reference, not reading: keep it short enough to take in "
		"at a glance, without scrolling. One line per role, in this order, and "
		"nothing else:\n"
		"差出人: <名前> <アドレス>\n"
		"宛先: <名前> <アドレス>, <名前> <アドレス>\n"
		"Cc: <名前> <アドレス>, ...\n"
		"Several people share one role line, comma-separated. Drop a role line "
		"entirely when that header is absent. Add a 本文中: line only for "
		"someone named in the body but absent from the headers, with their part "
		"in a few words. No bullets, no commentary, no blank lines.\n"
		"The same person may be written differently by different writers - "
		"family name only, given name only, nicknames, honorifics - and Japanese "
		"names have a family name and a given name. Treat variants as one "
		"person: give the full name once and put the variant in parentheses "
		"right after it, e.g. 森本健志 <k@example.jp> (本文中では「森本さん」). "
		"Never spend a separate line on that.\n"
		"Reproduce every email address exactly as it appears, character for "
		"character. In particular never capitalise the first letter of an "
		"address because it happens to start a line or a sentence - an address "
		"is not a word, and 'Taro@example.jp' for 'taro@example.jp' is wrong.\n"
		"\n"
		"【メールの要約】\n"
		"Bullet points, following these rules:\n"
		"- A thread is ordered newest-first: the newly written message is at the "
		"top and older quoted messages sit below it. Your summary must run the "
		"opposite way - start from the oldest premise and end with the newest "
		"development.\n"
		"- The newest part matters most: it is what the sender is telling あなた "
		"right now, so give it the most detail - the most child bullets. The "
		"older history gets fewer child bullets, but it is neither dropped nor "
		"demoted: the earlier steps still belong on the top-level spine, "
		"because that is where the story starts.\n"
		"- Keep people in the sentences - who asked or told what to whom. This is "
		"correspondence between people, not an abstract document.\n"
		"- State the sender's intent for the newest message explicitly. Typical "
		"intents: 情報共有 / 対応依頼 / 意思決定・承認の要請 / 確認・合意形成 / "
		"相談・意見聴取 / 証跡・エビデンスの記録 / 関係構築・社交.\n"
		"- The exchange may be between two people or many. Say who is "
		"corresponding with whom. Some addressees only receive and never reply - "
		"do not assume every addressee is an active participant. Where the "
		"earlier thread's participants are not visible, assume they are the same "
		"as this message's.\n"
		"- The content has a logical structure (premise -> development -> "
		"request, or problem -> options -> decision). Reflect that structure in "
		"how the bullets nest and follow one another. Do not emit a flat list of "
		"disconnected facts.\n"
		"- Always carry through the concrete specifics that a decision turns on: "
		"deadlines and dates, amounts and quantities, locations, and proper nouns "
		"(system names, document names, product names). Never drop these for the "
		"sake of brevity.\n"
		"- Organise the bullets into levels. The top level is the spine of the "
		"story and runs in time order - oldest premise first, newest "
		"development last. Someone who reads ONLY the top-level bullets, "
		"skipping everything indented beneath them, must come away with a "
		"complete and correct understanding of this mail, in the order things "
		"happened. Aim for three to six of them.\n"
		"- Convey that ordering by the order alone. Never prefix a bullet with "
		"a label such as 「前提:」「最新:」「背景:」「結論:」. Its position in the "
		"list already says which it is, and the label spends words the bullet "
		"needs for content. 'Premise' and 'development' above describe how to "
		"sequence the bullets; they are not tags to write out.\n"
		"- Indent the supporting material under the bullet it belongs to - the "
		"specifics, the evidence, the background, who said what. Two spaces per "
		"level, at most three levels. A flat list of equally weighted bullets is "
		"a failure: it forces the reader to read everything to find out what "
		"matters.\n"
		"- One idea per bullet, about one sentence. Split a bullet that needs "
		"two sentences rather than running them together.\n"
		"- 'Three to six' is a target for how the story usually decomposes, not "
		"a quota. If the mail genuinely contains more distinct developments, "
		"list them all.\n"
		"\n"
		"These rules pull against each other. When they do, resolve them in "
		"this order:\n"
		"1. Lose nothing material. Every request, decision, commitment, "
		"question, condition, date, number and name that a reader would act on "
		"or be judged on must survive into the summary somewhere.\n"
		"2. Keep the top level in time order.\n"
		"3. Then be brief.\n"
		"Brevity is achieved by moving detail DEEPER in the hierarchy, never by "
		"leaving it out. A summary that is short because it omitted something "
		"is not a good summary; it is a wrong one.\n"
		"\n"
		"【メールへの推奨対応】\n"
		"The reader wants the conclusion first. Structure this section exactly "
		"as follows.\n"
		"\n"
		"The first line states whether あなた has to do anything, as one short "
		"sentence and nothing else: 「対応は不要です」 or 「対応が必要です」. Use "
		"「至急対応が必要です」 instead when a deadline is near or already past - "
		"judge from the mail's Date header and the current date/time, both given "
		"above. Never label the ordinary case: a priority of 'normal' tells the "
		"reader nothing, so it must not appear.\n"
		"\n"
		"If nothing is needed: follow with the reason in one or two sentences. "
		"Do not use bullets - there is nothing to enumerate.\n"
		"\n"
		"If something is needed: list the actions as top-level bullets, one "
		"action per bullet, each a short imperative phrase saying WHAT to do - "
		"not why, not the background. Put the explanation, the grounds and any "
		"history behind an action in child bullets indented under it. For each "
		"action say whether it is a reply to this mail or something to be done "
		"outside mail (and if outside, what). Give any deadline that applies, "
		"and say plainly if it has already passed.\n"
		"\n"
		"Output only these three sections. No preamble, no closing remarks, no "
		"markdown fences.";
	// 8192: a hierarchical summary forbidden to drop anything runs long, and
	// Japanese costs roughly a token per character. Output tokens are billed
	// only when produced, so headroom is free - whereas hitting the cap
	// truncates the answer mid-sentence, which loses the third section
	// heading and makes the whole reply fall into the summary pane.
	RunMsgViewToResultDialog(hWnd, L"要約 - Becky! Claude Plugin", "summarize", systemPrompt, 8192, true, true);
}

void CALLBACK OnCmdTranslateMailToJapanese(HWND hWnd, LPARAM /*lParam*/)
{
	std::string systemPrompt =
		"Translate this email into natural, fluent Japanese. The source "
		"language may be anything (English, French, Chinese, etc.); if it is "
		"already Japanese, output it unchanged.\n"
		"This is NOT a mechanical, word-by-word translation. Read the whole "
		"message - including any quoted earlier messages in the thread - and "
		"use that context to disambiguate word senses and pick the wording a "
		"skilled bilingual translator would choose. e.g. render 'game' as 試合 "
		"in a sports context but ゲーム / ゲームソフト in a video-game context, "
		"judging from what the exchange is actually about. Where an ongoing "
		"back-and-forth gives extra context, use it to make the Japanese more "
		"precise and natural.\n"
		"Translate the ENTIRE content, quoted history included, so the reader "
		"understands the full exchange. Keep quoted parts clearly marked as "
		"quotes (preserve their '>' markers or an equivalent indication). Do "
		"not summarize or omit anything. Preserve paragraph structure and "
		"line breaks.\n"
		"Output ONLY the translated (or unchanged) Japanese text - no "
		"commentary, no translator's notes, no markdown.";
	// A translation runs about as long as its source, and the source is
	// capped at kMaxBodyChars - roughly 4000 Japanese characters, i.e. some
	// 4000 tokens. 4096 left no margin at all.
	RunMsgViewToResultDialog(hWnd, L"翻訳 - Becky! Claude Plugin", "translate", systemPrompt, 8192);
}

#pragma once
#include <string>
#include <vector>

struct PluginSettings
{
	std::string model = "claude-haiku-4-5";
	int summarizeCacheMinutes = 1440; // 24h; 0 disables the summarize result cache
	int translateCacheMinutes = 1440; // 24h; 0 disables the translate result cache

	// Style learning walks the Sent folder newest-first, so both of these are
	// stopping conditions and whichever hits first ends the scan. The count
	// bounds cost and guarantees a usable sample size; the age limit keeps an
	// old register out of the profile.
	// 200 rather than a smaller number because the profile now states rules
	// per recipient address: what matters is samples per correspondent, not
	// the total, and thinly-covered recipients are deliberately dropped.
	int learnMaxMails = 200;
	int learnMaxMonths = 0; // 0 = no age limit

	// Warn at send time when the body announces an attachment and none is
	// there. This governs the send-time check only: the phrases are learned
	// either way, so switching it back on takes effect immediately rather
	// than silently doing nothing until the Sent folder is learned again.
	bool warnMissingAttachment = true;

	// Whatever the user typed into the ライセンスキー field. Stored as typed,
	// unmasked; see IsLicenseKeyValid for what it is compared against and
	// why nothing depends on the answer.
	std::string licenseKey;
};

// The plug-in presents itself as shareware, in the manner of the Japanese
// shareware of the 1990s, and its key - MORIMOTO=4000YEN, after the
// HIDEMARU=4000YEN of legend - is printed in the README for everyone to use,
// "until the price is decided". Entering it changes exactly one thing: the
// 状態 line in the settings dialog reads 登録済み instead of 未登録. No
// feature checks it, nothing nags, nothing is limited. The licence that
// actually applies is MIT. This function exists so the joke has a punchline.
bool IsLicenseKeyValid(const std::string& key);

std::string GetPluginDataDir();
bool LoadSettings(PluginSettings& out);
bool SaveSettings(const PluginSettings& in);

bool HasApiKey();
bool LoadApiKey(std::string& outKey);
bool SaveApiKey(const std::string& key);

// A Sent folder the user has pointed at, one per mailbox. Style is learned
// per mailbox because a personal account and a role account are written in
// noticeably different registers.
struct SentFolderInfo
{
	std::string folderId;    // e.g. "1234abcd.mb\\送信済み\\"
	std::string learnedAt;   // "YYYY-MM-DD HH:MM", empty if never learned
	int sampleCount = 0;
	// Which model wrote the profile. The profile is plain Japanese prose, so
	// any model can use one another model produced - but its quality depends
	// on the model that wrote it, and without this there is no way to tell
	// what the stored profile came from.
	std::string learnedModel;
};

// "1234abcd.mb\送信済み\" -> "1234abcd.mb\"
std::string MailboxIdFromFolderId(const std::string& folderId);

std::vector<SentFolderInfo> LoadSentFolders();
void SaveSentFolders(const std::vector<SentFolderInfo>& folders);

// Style profiles are plain UTF-8 text, one file per mailbox; not secret, so
// no DPAPI encryption.
bool LoadStyleProfile(const std::string& mailboxId, std::string& outUtf8Text);
bool SaveStyleProfile(const std::string& mailboxId, const std::string& utf8Text);
bool DeleteStyleProfile(const std::string& mailboxId);

// The phrases this mailbox uses to announce an attachment, learned from the
// same Sent folder as the style profile: one per line, UTF-8, no other syntax.
//
// A separate file from the style profile because the two are read by different
// readers. The profile is prose for the model to follow; this list is matched
// literally against the body at send time, so it has to stay free of prose.
// Being a plain line list also makes it directly editable: a phrase that turns
// out to fire on mails with nothing attached can be deleted by hand, which is
// a more precise fix than learning the folder again.
bool LoadAttachPhrases(const std::string& mailboxId, std::vector<std::string>& outPhrases);
bool SaveAttachPhrases(const std::string& mailboxId, const std::string& utf8Lines);
bool DeleteAttachPhrases(const std::string& mailboxId);

// Every signature configured in every mailbox, as UTF-8 text. Becky! keeps
// them as "$$$<NNNNN><name>.txt" (and a ".txt.utf8" twin) in each mailbox
// folder; a mailbox may have none or several. All mailboxes are read rather
// than just the relevant one because a new mail does not reliably say which
// account it belongs to - and an exact text match cannot false-positive.
std::vector<std::string> LoadAllSignatures();

// The mailbox that sends from `address` ("1234abcd.mb\"), or empty if none
// matches. Becky! records each account's own address in its Mailbox.ini, so
// a compose window's From header identifies its mailbox exactly - which is
// how a new mail, having no reference to any existing mail, can still be
// tied to the right account.
std::string MailboxIdForAddress(const std::string& address);

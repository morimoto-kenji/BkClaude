#pragma once
#include <string>
#include <vector>

// Pulls the plain-text body out of a raw RFC822 message source (what
// CBeckyAPI::GetSource returns) and returns it as UTF-8.
//
// Deliberately only looks at the TOP-LEVEL text/plain part - descending one
// level into a multipart/alternative, but never into a message/rfc822
// attachment. An attached mail is somebody else's writing, and the whole
// point of reading these is to learn the account owner's own style.
// Returns an empty string when there is no usable top-level text part.
std::string ExtractPlainTextBodyUtf8(const std::string& rawSource);

// Cuts everything from the first inline forward/quote separator onward
// ("-----Original Message-----", a rule of underscores, "... wrote:",
// "...さんは書きました" and friends). Text below those lines was written by
// someone else even though it carries no '>' markers.
std::string StripForwardedTail(const std::string& utf8Text);

// One top-level header field, unfolded and undecoded (raw). Enough for
// structural fields like "Date"; not for anything carrying encoded-words.
std::string GetTopLevelHeader(const std::string& rawSource, const std::string& name);

// True when the message carries a file attachment.
//
// Keyed on "Content-Disposition: attachment" rather than on the message being
// multipart: an HTML mail is multipart/alternative with nothing attached, and
// an inline image is multipart/related with Content-Disposition: inline. This
// is the only marker that means a file, and Becky! has already written it by
// the time BKC_OnOutgoing runs (verified against real sends, with and
// without an attachment).
bool SourceHasAttachment(const std::string& rawSource);

// Offset where the part of a composed message that must be preserved
// verbatim begins - the quoted original, or a signature block, whichever
// comes first - or npos when the whole body is the user's own writing.
//
// Taking the earlier of the two covers every arrangement with one rule: a new
// mail (signature only), a reply with the signature below the quote, and a
// reply with it above. Everything before the offset is what the user wrote;
// everything from it on is passed through untouched instead of being sent
// through the model, so it cannot come back altered or be replaced away.
// `signatures` are the mailbox signatures to look for verbatim (see
// LoadAllSignatures). A signature that is registered in Becky! is matched
// exactly; one that is not falls back to the RFC 3676 "-- " delimiter.
size_t FindPreservedTailStart(const std::string& utf8Text,
                               const std::vector<std::string>& signatures);

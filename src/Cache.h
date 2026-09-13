#pragma once
#include <string>

// Per-mail result cache, keyed by feature + variant + mail ID. cacheMinutes
// <= 0 disables caching for that feature (LoadCachedResult always misses;
// SaveCachedResult is a no-op). Expired entries of the given feature are
// pruned lazily whenever that feature's cache is touched, so it never grows
// without bound. Each feature ("summarize", "translate") is independent.
//
// `variant` is everything besides the mail that determines the answer - the
// model and a fingerprint of the prompt (see CacheVariant). Without it,
// switching models or editing a prompt silently returns the old answer, which
// looks exactly like the change having no effect.
bool LoadCachedResult(const std::string& feature, const std::string& variant, const std::string& mailId, int cacheMinutes, std::string& outUtf8Text);
void SaveCachedResult(const std::string& feature, const std::string& variant, const std::string& mailId, const std::string& utf8Text, int cacheMinutes);

// model + a hash of the prompt, as one opaque key component.
std::string CacheVariant(const std::string& model, const std::string& systemPrompt, int maxTokens);

// Deletes all cached results for one feature. Returns the number of entries
// removed, or -1 if the cache file could not be rewritten.
int ClearCache(const std::string& feature);

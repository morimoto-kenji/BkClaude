#include <windows.h>
#include <fstream>
#include <ctime>
#include <cstdio>
#include <vector>
#include "Cache.h"
#include "Settings.h"
#include "json.hpp"

using json = nlohmann::json;

namespace {

std::string CacheFilePath()
{
	std::string dir = GetPluginDataDir();
	if (!dir.empty() && dir.back() != '\\') dir += '\\';
	return dir + "result_cache.json";
}

json LoadCacheFile()
{
	std::ifstream f(CacheFilePath(), std::ios::binary);
	if (!f.good()) return json::object();
	std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
	if (content.empty()) return json::object();
	try {
		return json::parse(content);
	} catch (...) {
		return json::object();
	}
}

bool SaveCacheFile(const json& j)
{
	std::ofstream f(CacheFilePath(), std::ios::binary | std::ios::trunc);
	if (!f.good()) return false;
	std::string dump = j.dump();
	f.write(dump.data(), (std::streamsize)dump.size());
	return f.good();
}

std::string KeyPrefix(const std::string& feature)
{
	return feature + ":";
}

std::string MakeKey(const std::string& feature, const std::string& variant, const std::string& mailId)
{
	// The feature prefix stays leftmost so pruning and ClearCache still match
	// every variant of a feature with one prefix test.
	return KeyPrefix(feature) + variant + ":" + mailId;
}

// Removes expired entries of `feature` only; other features are left alone.
void PruneExpired(json& entries, const std::string& feature, int cacheMinutes)
{
	std::string prefix = KeyPrefix(feature);
	long long cutoff = (long long)time(NULL) - (long long)cacheMinutes * 60;
	std::vector<std::string> toErase;
	for (auto it = entries.begin(); it != entries.end(); ++it) {
		if (it.key().compare(0, prefix.size(), prefix) != 0) continue;
		long long ts = it.value().value("timestamp", (long long)0);
		if (cacheMinutes <= 0 || ts < cutoff) toErase.push_back(it.key());
	}
	for (auto& k : toErase) entries.erase(k);
}

} // namespace

std::string CacheVariant(const std::string& model, const std::string& systemPrompt, int maxTokens)
{
	// FNV-1a over the prompt. A hash rather than a hand-maintained version
	// number: a number only works if it is remembered every single time the
	// prompt changes, and the failure mode of forgetting - stale answers that
	// look like the edit did nothing - is the very thing this prevents.
	unsigned long long h = 1469598103934665603ULL;
	for (unsigned char c : systemPrompt) {
		h ^= c;
		h *= 1099511628211ULL;
	}
	h ^= (unsigned long long)maxTokens;
	h *= 1099511628211ULL;

	char buf[32];
	sprintf(buf, "%016llx", h);
	return model + "@" + buf;
}

bool LoadCachedResult(const std::string& feature, const std::string& variant, const std::string& mailId, int cacheMinutes, std::string& outUtf8Text)
{
	if (cacheMinutes <= 0 || mailId.empty()) return false;

	json root = LoadCacheFile();
	json entries = root.value("entries", json::object());
	PruneExpired(entries, feature, cacheMinutes);

	std::string key = MakeKey(feature, variant, mailId);
	auto it = entries.find(key);
	bool hit = it != entries.end();
	if (hit) {
		outUtf8Text = it.value().value("text", "");
		hit = !outUtf8Text.empty();
	}

	root["entries"] = entries;
	SaveCacheFile(root); // persist pruning even on a miss
	return hit;
}

void SaveCachedResult(const std::string& feature, const std::string& variant, const std::string& mailId, const std::string& utf8Text, int cacheMinutes)
{
	if (cacheMinutes <= 0 || mailId.empty()) return;

	json root = LoadCacheFile();
	json entries = root.value("entries", json::object());
	PruneExpired(entries, feature, cacheMinutes);

	json entry;
	entry["timestamp"] = (long long)time(NULL);
	entry["text"] = utf8Text;
	entries[MakeKey(feature, variant, mailId)] = entry;

	root["entries"] = entries;
	SaveCacheFile(root);
}

int ClearCache(const std::string& feature)
{
	json root = LoadCacheFile();
	json entries = root.value("entries", json::object());

	std::string prefix = KeyPrefix(feature);
	std::vector<std::string> toErase;
	for (auto it = entries.begin(); it != entries.end(); ++it) {
		if (it.key().compare(0, prefix.size(), prefix) == 0) toErase.push_back(it.key());
	}
	for (auto& k : toErase) entries.erase(k);

	int removed = (int)toErase.size();
	if (removed == 0) return 0; // nothing to write

	root["entries"] = entries;
	return SaveCacheFile(root) ? removed : -1;
}

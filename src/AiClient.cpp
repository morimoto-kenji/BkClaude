#include <windows.h>
#include <winhttp.h>
#include "AiClient.h"
#include "Encoding.h"
#include "json.hpp"

using json = nlohmann::json;

namespace {

struct HttpResult
{
	bool transportOk = false;   // the request reached a server and got a reply
	DWORD statusCode = 0;
	std::string body;
	std::string transportError;
};

// "12029" on its own tells the reader nothing. WinHTTP's codes (12001-12186)
// are not in the system message table - they live in winhttp.dll - so the
// module has to be named explicitly or FormatMessage returns nothing. The
// text comes back in the system language, which is why this needs no
// translation table of its own.
std::string DescribeWinError(DWORD err)
{
	std::string out = "Windows error " + std::to_string(err);

	LPWSTR buf = NULL;
	DWORD len = FormatMessageW(
		FORMAT_MESSAGE_FROM_HMODULE | FORMAT_MESSAGE_FROM_SYSTEM |
		FORMAT_MESSAGE_IGNORE_INSERTS | FORMAT_MESSAGE_ALLOCATE_BUFFER,
		GetModuleHandleW(L"winhttp.dll"), err, 0, (LPWSTR)&buf, 0, NULL);
	if (len && buf) {
		std::wstring w(buf, len);
		while (!w.empty() && (w.back() == L'\r' || w.back() == L'\n' || w.back() == L' ')) {
			w.pop_back();
		}
		if (!w.empty()) out += ": " + WideToUtf8(w);
	}
	if (buf) LocalFree(buf);
	return out;
}

// Every error this file reports has the same shape: a Japanese line saying
// what to do, then a blank line, then the raw fact - the API's own message
// with its HTTP status, or the Windows error with its code. The two answer
// different questions. The first is for the person at the keyboard, who needs
// to know whether to fix the key, wait, or check the network; the second is
// for whoever investigates later, and is the only part that can be searched
// for or quoted to support. Replacing one with the other would lose the
// other's reader.
std::string WithGuidance(const std::string& guidance, const std::string& detail)
{
	return guidance + "\r\n\r\n" + detail;
}

const char* kSettingsPath = "「ツール」→「プラグインの設定」";

// WinHTTP failed before any response arrived. A few codes cover nearly every
// real case, and each points at a different thing to check.
std::string TransportFailure(const char* stage, DWORD err)
{
	std::string guidance;
	switch (err) {
	case ERROR_WINHTTP_TIMEOUT:
		guidance = "応答がありませんでした（タイムアウト）。ネットワークが遅いか、"
		           "サーバーの応答に時間がかかっています。しばらく待ってから再試行してください。";
		break;
	case ERROR_WINHTTP_NAME_NOT_RESOLVED:
		guidance = "api.anthropic.com の名前解決に失敗しました。"
		           "インターネット接続と DNS の設定を確認してください。";
		break;
	case ERROR_WINHTTP_CANNOT_CONNECT:
		guidance = "api.anthropic.com に接続できませんでした。"
		           "インターネット接続、ファイアウォール、プロキシの設定を確認してください。";
		break;
	case ERROR_WINHTTP_SECURE_FAILURE:
	case ERROR_WINHTTP_CLIENT_AUTH_CERT_NEEDED:
		guidance = "HTTPS 接続を確立できませんでした。証明書を差し替えるプロキシや"
		           "セキュリティソフトが介在していないか確認してください。";
		break;
	default:
		guidance = "Anthropic への通信に失敗しました。"
		           "インターネット接続とプロキシの設定を確認してください。";
		break;
	}
	std::string detail = DescribeWinError(err);
	if (stage && *stage) detail = std::string(stage) + ": " + detail;
	return WithGuidance(guidance, detail);
}

// The receive timeout has to cover the whole generation, not just the network.
// These requests do not stream, so the server sends nothing at all until the
// last token is written: WinHttpReceiveResponse sits there for as long as the
// model takes. A fixed 60s was therefore a cap on how much output could ever
// be asked for, and raising max_tokens turned it into a timeout (12002) rather
// than a longer wait. Callers pass a budget derived from what they requested.
HttpResult HttpRequest(LPCWSTR method, LPCWSTR path, const std::string& apiKey,
                        const std::string& jsonBody /* empty = no body */,
                        DWORD receiveTimeoutMs)
{
	HttpResult r;

	HINTERNET hSession = WinHttpOpen(L"BeckyAiPlugin/0.1",
		WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
		WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
	if (!hSession) {
		r.transportError = TransportFailure("WinHttpOpen", GetLastError());
		return r;
	}
	WinHttpSetTimeouts(hSession, 10000, 10000, 30000, (int)receiveTimeoutMs);

	HINTERNET hConnect = WinHttpConnect(hSession, L"api.anthropic.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
	if (!hConnect) {
		r.transportError = TransportFailure("WinHttpConnect", GetLastError());
		WinHttpCloseHandle(hSession);
		return r;
	}

	HINTERNET hRequest = WinHttpOpenRequest(hConnect, method, path,
		NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
	if (!hRequest) {
		r.transportError = TransportFailure("WinHttpOpenRequest", GetLastError());
		WinHttpCloseHandle(hConnect);
		WinHttpCloseHandle(hSession);
		return r;
	}

	std::wstring headers = L"anthropic-version: 2023-06-01\r\n"
	                        L"x-api-key: " + Utf8ToWide(apiKey) + L"\r\n";
	if (!jsonBody.empty()) {
		headers += L"content-type: application/json\r\n";
	}

	BOOL bSent = WinHttpSendRequest(hRequest,
		headers.c_str(), (DWORD)headers.size(),
		jsonBody.empty() ? WINHTTP_NO_REQUEST_DATA : (LPVOID)jsonBody.data(),
		(DWORD)jsonBody.size(), (DWORD)jsonBody.size(), 0);

	if (bSent && WinHttpReceiveResponse(hRequest, NULL)) {
		DWORD size = sizeof(r.statusCode);
		WinHttpQueryHeaders(hRequest,
			WINHTTP_QUERY_FLAG_NUMBER | WINHTTP_QUERY_STATUS_CODE,
			WINHTTP_HEADER_NAME_BY_INDEX, &r.statusCode, &size, WINHTTP_NO_HEADER_INDEX);

		DWORD dwAvail = 0;
		do {
			dwAvail = 0;
			if (!WinHttpQueryDataAvailable(hRequest, &dwAvail)) break;
			if (dwAvail == 0) break;
			std::string chunk(dwAvail, 0);
			DWORD dwRead = 0;
			if (!WinHttpReadData(hRequest, &chunk[0], dwAvail, &dwRead)) break;
			chunk.resize(dwRead);
			r.body += chunk;
		} while (dwAvail > 0);

		r.transportOk = true;
	} else {
		r.transportError = TransportFailure(NULL, GetLastError());
	}

	WinHttpCloseHandle(hRequest);
	WinHttpCloseHandle(hConnect);
	WinHttpCloseHandle(hSession);
	return r;
}

// The server answered, with an error. The status decides what the user can
// do about it; the body's own message says why, in English, and is kept.
std::string DescribeApiError(const std::string& body, DWORD statusCode)
{
	std::string apiMessage, apiType;
	try {
		json j = json::parse(body);
		if (j.contains("error")) {
			apiMessage = j["error"].value("message", "");
			apiType = j["error"].value("type", "");
		}
	} catch (...) {
		apiMessage = "invalid response body";
	}

	std::string lower = apiMessage;
	for (char& c : lower) if (c >= 'A' && c <= 'Z') c += ('a' - 'A');

	std::string guidance;
	switch (statusCode) {
	case 401:
		guidance = "API キーが無効です。" + std::string(kSettingsPath) +
		           " でキーを確認してください。";
		break;
	case 403:
		guidance = "この API キーには権限がありません。Anthropic Console で"
		           "キーの権限と所属組織を確認してください。";
		break;
	case 404:
		guidance = "指定したモデルが見つかりません。" + std::string(kSettingsPath) +
		           " でモデルを確認してください。";
		break;
	case 400:
		// One 400 is not like another. Billing is the one a user can fix
		// without touching the plug-in, so it gets its own line; the rest are
		// request problems, and the API's message is the useful part.
		if (lower.find("credit") != std::string::npos ||
		    lower.find("billing") != std::string::npos ||
		    lower.find("balance") != std::string::npos) {
			guidance = "クレジット残高が不足しています。Anthropic Console の"
			           "請求設定を確認してください。";
		} else {
			guidance = "リクエストが拒否されました。下の詳細を確認してください。"
			           "モデルの設定が原因の場合は " + std::string(kSettingsPath) +
			           " で変更できます。";
		}
		break;
	case 413:
		guidance = "送信内容が大きすぎます。メール本文が長すぎる可能性があります。";
		break;
	case 429:
		guidance = "利用制限に達しました。しばらく待ってから再試行してください。";
		break;
	case 529:
		guidance = "Anthropic の API が混雑しています。しばらく待ってから再試行してください。";
		break;
	default:
		if (statusCode >= 500) {
			guidance = "Anthropic 側で一時的な障害が発生しています。"
			           "しばらく待ってから再試行してください。";
		} else {
			guidance = "Anthropic からエラーが返されました。下の詳細を確認してください。";
		}
		break;
	}

	std::string detail = "HTTP " + std::to_string(statusCode);
	if (!apiType.empty()) detail += " " + apiType;
	if (!apiMessage.empty()) detail += ": " + apiMessage;
	return WithGuidance(guidance, detail);
}

} // namespace

AiResponse CallClaudeSync(const std::string& apiKey,
                           const std::string& model,
                           const std::string& systemPrompt,
                           const std::string& userPrompt,
                           int maxTokens,
                           int thinkingBudget)
{
	AiResponse result;

	// Without temperature the API default of 1.0 applies, i.e. sampled output
	// that differs every run. Nothing this plugin does is creative -
	// summarizing, translating, proofreading and drafting in a learned style
	// are all extraction and structuring tasks, where the same mail giving the
	// same answer is the point. (Not a determinism guarantee: server-side
	// variation still leaves a little room to move.)
	//
	// Newer models reject the parameter outright ("'temperature' is deprecated
	// for this model"); the retry below handles that.
	//
	// A deliberately pessimistic generation rate - slower than any model is
	// expected to be - so the wait is bounded by what was asked for rather
	// than by a number picked once and left. The floor keeps short requests
	// from becoming impatient; the ceiling stops a bad request from hanging
	// the worker thread indefinitely.
	DWORD receiveTimeoutMs = 30000 + (DWORD)maxTokens * 1000 / 30;
	if (receiveTimeoutMs < 60000) receiveTimeoutMs = 60000;
	if (receiveTimeoutMs > 600000) receiveTimeoutMs = 600000;

	// temperature and extended thinking are mutually exclusive - a thinking
	// request must leave sampling at its default - so a caller that names a
	// budget has already given up determinism, and asking for both would only
	// earn a 400 on the first attempt.
	bool sendThinking = (thinkingBudget != kThinkDefault);
	bool sendTemperature = !sendThinking;

	HttpResult http;
	for (int attempt = 0; attempt < 3; attempt++) {
		std::string requestBody;
		try {
			json body;
			body["model"] = model;
			body["max_tokens"] = maxTokens;
			if (sendTemperature) {
				body["temperature"] = 0;
			}
			if (sendThinking) {
				body["thinking"] = (thinkingBudget > 0)
					? json{ {"type", "enabled"}, {"budget_tokens", thinkingBudget} }
					: json{ {"type", "disabled"} };
			}
			if (!systemPrompt.empty()) {
				body["system"] = systemPrompt;
			}
			body["messages"] = json::array({ { {"role", "user"}, {"content", userPrompt} } });
			requestBody = body.dump();
		} catch (const std::exception& e) {
			// dump() rejects malformed UTF-8; report it instead of letting the
			// exception escape the worker thread and abort the host process.
			result.error = WithGuidance(
				"送信データの作成に失敗しました。メール本文に不正な文字が含まれている可能性があります。",
				std::string("Failed to encode request: ") + e.what());
			return result;
		}

		http = HttpRequest(L"POST", L"/v1/messages", apiKey, requestBody, receiveTimeoutMs);
		if (!http.transportOk) {
			result.error = http.transportError;
			return result;
		}
		// Drop whichever optional parameter the API named and try again. Same
		// reasoning for both: keeping a table of which models accept what
		// would go stale as models come and go, and a rejected request costs
		// a 400 and no tokens.
		if (http.statusCode == 400) {
			if (sendTemperature && http.body.find("temperature") != std::string::npos) {
				sendTemperature = false;
				continue;
			}
			if (sendThinking && http.body.find("thinking") != std::string::npos) {
				sendThinking = false;
				continue;
			}
		}
		break;
	}

	if (http.statusCode != 200) {
		result.error = DescribeApiError(http.body, http.statusCode);
		return result;
	}

	try {
		json j = json::parse(http.body);
		std::string text;
		if (j.contains("content") && j["content"].is_array()) {
			for (auto& block : j["content"]) {
				if (block.value("type", "") == "text") {
					text += block.value("text", "");
				}
			}
		}
		// A run into max_tokens is reported as an error rather than returned
		// as partial text. Truncated output is not merely shorter - a summary
		// loses its last section, and a draft would be written into the
		// compose window ending mid-sentence. Failing also keeps it out of
		// the result cache, so a retry is not served the same broken answer.
		if (j.value("stop_reason", "") == "max_tokens") {
			// The counts are in the message on purpose. "It got cut off" alone
			// leaves no way to tell a genuinely long answer from a limit set
			// too low, or from output tokens being spent on something other
			// than the text we asked for - and that difference decides whether
			// the fix is a bigger limit or a different prompt. The block types
			// are here for the same reason: they say what the model actually
			// produced, which need not all be text.
			int inTok = 0, outTok = 0;
			if (j.contains("usage")) {
				inTok = j["usage"].value("input_tokens", 0);
				outTok = j["usage"].value("output_tokens", 0);
			}
			std::string kinds;
			if (j.contains("content") && j["content"].is_array()) {
				for (auto& block : j["content"]) {
					std::string t = block.value("type", "?");
					if (kinds.find(t) != std::string::npos) continue;
					if (!kinds.empty()) kinds += "+";
					kinds += t;
				}
			}

			result.error = "応答が出力上限に達し、途中で切れました。\r\n"
			               "上限 " + std::to_string(maxTokens) +
			               " / 出力 " + std::to_string(outTok) +
			               " / 入力 " + std::to_string(inTok) + " トークン";
			if (!kinds.empty()) result.error += " / 内容 " + kinds;
			result.error += "\r\n受け取った本文 " + std::to_string(text.size()) + " バイト";
			return result;
		}

		result.success = true;
		result.text = text;
	} catch (...) {
		result.error = WithGuidance(
			"Anthropic からの応答を読み取れませんでした。再試行しても続く場合は、"
			"プロキシが応答を書き換えていないか確認してください。",
			"HTTP 200: invalid response body");
	}
	return result;
}

ModelsResponse ListModelsSync(const std::string& apiKey)
{
	ModelsResponse result;

	// A fixed list, generated by nothing: the plain network timeout applies.
	HttpResult http = HttpRequest(L"GET", L"/v1/models?limit=100", apiKey, "", 60000);
	if (!http.transportOk) {
		result.error = http.transportError;
		return result;
	}
	if (http.statusCode != 200) {
		result.error = DescribeApiError(http.body, http.statusCode);
		return result;
	}

	try {
		json j = json::parse(http.body);
		if (j.contains("data") && j["data"].is_array()) {
			for (auto& m : j["data"]) {
				ModelInfo info;
				info.id = m.value("id", "");
				info.displayName = m.value("display_name", "");
				info.createdAt = m.value("created_at", "");
				if (!info.id.empty()) {
					result.models.push_back(info);
				}
			}
		}
		result.success = true;
	} catch (...) {
		result.error = WithGuidance(
			"Anthropic からの応答を読み取れませんでした。再試行しても続く場合は、"
			"プロキシが応答を書き換えていないか確認してください。",
			"HTTP 200: invalid response body");
	}
	return result;
}

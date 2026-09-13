#include "AsyncRunner.h"
#include "AiClient.h"
#include <thread>

namespace {

const wchar_t* kClassName = L"BeckyAiPluginAsyncWnd";
HWND g_hWnd = NULL;
const UINT WM_AI_RESULT = WM_APP + 1;
const UINT WM_AI_DONE = WM_APP + 2;

struct AsyncContext
{
	AiResponse response;
	AiResultCallback callback;
};

LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	if (msg == WM_AI_RESULT) {
		AsyncContext* ctx = (AsyncContext*)lParam;
		if (ctx->callback) {
			ctx->callback(ctx->response.success, ctx->response.text, ctx->response.error);
		}
		delete ctx;
		return 0;
	}
	if (msg == WM_AI_DONE) {
		auto* done = (std::function<void()>*)lParam;
		if (*done) (*done)();
		delete done;
		return 0;
	}
	return DefWindowProc(hWnd, msg, wParam, lParam);
}

} // namespace

void AsyncRunner_Init(HINSTANCE hInstance)
{
	if (g_hWnd) return;

	WNDCLASSW wc{};
	wc.lpfnWndProc = WndProc;
	wc.hInstance = hInstance;
	wc.lpszClassName = kClassName;
	RegisterClassW(&wc);

	g_hWnd = CreateWindowW(kClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, hInstance, NULL);
}

void AsyncRunner_CallClaude(const std::string& apiKey,
                             const std::string& model,
                             const std::string& systemPrompt,
                             const std::string& userPrompt,
                             int maxTokens,
                             AiResultCallback callback,
                             int thinkingBudget)
{
	// Nothing may escape a worker thread: an unhandled exception here would
	// terminate Becky! itself, not just the plugin.
	std::thread([apiKey, model, systemPrompt, userPrompt, maxTokens, thinkingBudget, callback]() {
		AiResponse resp;
		try {
			resp = CallClaudeSync(apiKey, model, systemPrompt, userPrompt, maxTokens, thinkingBudget);
		} catch (const std::exception& e) {
			resp.success = false;
			resp.error = "プラグイン内部でエラーが発生しました。\r\n\r\n"
			             "Internal error: " + std::string(e.what());
		} catch (...) {
			resp.success = false;
			resp.error = "プラグイン内部でエラーが発生しました。\r\n\r\n"
			             "Internal error (unknown exception)";
		}
		AsyncContext* ctx = new AsyncContext{ resp, callback };
		PostMessage(g_hWnd, WM_AI_RESULT, 0, (LPARAM)ctx);
	}).detach();
}

void AsyncRunner_RunLater(std::function<void()> fn)
{
	if (!g_hWnd || !fn) return;
	auto* heapFn = new std::function<void()>(fn);
	PostMessage(g_hWnd, WM_AI_DONE, 0, (LPARAM)heapFn);
}

void AsyncRunner_Post(std::function<void()> work, std::function<void()> done)
{
	std::thread([work, done]() {
		try {
			if (work) work();
		} catch (...) {
			// Swallow; `done` reports whatever state `work` managed to set.
		}
		auto* heapDone = new std::function<void()>(done);
		PostMessage(g_hWnd, WM_AI_DONE, 0, (LPARAM)heapDone);
	}).detach();
}

#pragma once
#include <windows.h>
#include <string>
#include <functional>
#include "AiClient.h"

void AsyncRunner_Init(HINSTANCE hInstance);

typedef std::function<void(bool success, const std::string& text, const std::string& error)> AiResultCallback;

// Runs CallClaudeSync on a worker thread; callback fires on the UI thread
// once the HTTP round-trip completes, so it is safe to touch Becky!/dialog
// windows from inside it.
void AsyncRunner_CallClaude(const std::string& apiKey,
                             const std::string& model,
                             const std::string& systemPrompt,
                             const std::string& userPrompt,
                             int maxTokens,
                             AiResultCallback callback,
                             int thinkingBudget = kThinkDefault);

// Generic: runs `work` on a worker thread, then runs `done` on the UI
// thread. Share state between them via a captured shared_ptr.
void AsyncRunner_Post(std::function<void()> work, std::function<void()> done);

// Runs `fn` on the UI thread from a plain top-level message dispatch, once
// the current one has fully unwound. Use it to get out of a Becky! callback
// before calling back into Becky! - re-entering it from inside its own menu
// command handling crashes B2.exe.
void AsyncRunner_RunLater(std::function<void()> fn);

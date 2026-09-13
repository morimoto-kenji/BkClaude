#pragma once
#include <string>
#include <vector>

struct AiResponse
{
	bool success = false;
	std::string text;
	std::string error;
};

// How much of `maxTokens` the model may spend on reasoning before it writes
// the answer. Reasoning output is billed and counted against the same budget
// as the answer, and on the current models it is substantial - a style card
// of 1,300 tokens was reached only after about 7,000 tokens of reasoning - so
// leaving it to the model turns max_tokens into a number that governs
// something other than the answer's length.
//
// kThinkDefault sends nothing and lets the model decide, which is right where
// the request has room to spare. Name a budget where it does not: the answer
// then has (maxTokens - budget) tokens guaranteed to itself.
const int kThinkDefault = -1;
const int kThinkOff = 0;

// Synchronous call to the Claude Messages API. Runs an HTTPS round-trip;
// call from a worker thread, never from the UI thread (see AsyncRunner).
AiResponse CallClaudeSync(const std::string& apiKey,
                           const std::string& model,
                           const std::string& systemPrompt,
                           const std::string& userPrompt,
                           int maxTokens,
                           int thinkingBudget = kThinkDefault);

struct ModelInfo
{
	std::string id;
	std::string displayName;
	std::string createdAt; // ISO-8601 string from the API
};

struct ModelsResponse
{
	bool success = false;
	std::string error;            // HTTP status + message on failure
	std::vector<ModelInfo> models; // first page only; enough for our use
};

// GET /v1/models - lists the models this key/org can access. No inference,
// no token cost. Also serves as a lightweight key-validity check.
// Call from a worker thread (see AsyncRunner).
ModelsResponse ListModelsSync(const std::string& apiKey);

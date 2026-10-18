# Using the console from coding tools

The app's llama-server speaks three APIs on port 47281, so most coding agents
can use the model on the PS5 as their backend:

| API | Path | Used by |
|---|---|---|
| OpenAI Chat Completions | `/v1/chat/completions` | opencode, aider, Continue, most others |
| OpenAI Responses | `/v1/responses` | codex |
| Anthropic Messages | `/v1/messages` | Claude Code |

Replace `192.168.1.19` with your console's address (the dashboard shows it).
The model name in a request is ignored: the server answers with the model
loaded in the library, so any name works. Tool calling uses the model's own
chat template (llama-server's Jinja templates are on by default).

## Which model

Agents send long prompts (system prompt, tool definitions, files), and
prompt processing on the console runs at 35 to 50 tokens a second for the
27B. A 20,000-token agent prompt takes several minutes before the first word.
For agents, a smaller model with a long context is the better trade:

- Qwen 3.8 27B: best answers, slow on long prompts; keep requests small.
- Gemma 4 E4B or another 4 to 9B model: several times faster on prompts.

Give the tool the context length the dashboard shows (the CONTEXT tile), so it
does not send more than the server holds.

## opencode

`~/.config/opencode/opencode.json`:

```json
{
  "$schema": "https://opencode.ai/config.json",
  "provider": {
    "ps5lm": {
      "npm": "@ai-sdk/openai-compatible",
      "name": "ArtemisLM PS5",
      "options": { "baseURL": "http://192.168.1.19:47281/v1" },
      "models": { "ps5": { "name": "Model on the PS5", "limit": { "context": 65536, "output": 8192 } } }
    }
  }
}
```

Then `opencode`, and pick **ArtemisLM PS5 / Model on the PS5** with `/models`. The
first run installs the `@ai-sdk/openai-compatible` package, which takes a few
minutes before anything reaches the console.

## codex

`~/.codex/config.toml`:

```toml
[model_providers.ps5lm]
name = "ArtemisLM PS5"
base_url = "http://192.168.1.19:47281/v1"
wire_api = "responses"

[profiles.ps5]
model_provider = "ps5lm"
model = "ps5"
```

Then `codex -p ps5`.

## Claude Code

```sh
ANTHROPIC_BASE_URL=http://192.168.1.19:47281 ANTHROPIC_AUTH_TOKEN=ps5lm \
ANTHROPIC_MODEL=ps5 ANTHROPIC_SMALL_FAST_MODEL=ps5 \
CLAUDE_CODE_MAX_CONTEXT_TOKENS=65536 claude
```

Set `CLAUDE_CODE_MAX_CONTEXT_TOKENS` to the context the dashboard shows:
Claude Code does not know the model and otherwise assumes 200k. It sends some
system messages in the middle of a conversation, which Qwen's chat template
refuses ("System message must be at the beginning"); the app's llama-server
folds them into the first one (`patches/0004`).

## aider

```sh
aider --openai-api-base http://192.168.1.19:47281/v1 --openai-api-key ps5lm --model openai/ps5
```

## Anything else

Point an OpenAI-compatible client at `http://192.168.1.19:47281/v1` with any
API key and any model name.

The library port is 47282. It stays up when a model is loaded.

```sh
curl http://192.168.1.19:47282/v1/models
curl http://192.168.1.19:47282/artemis
curl http://192.168.1.19:47282/load -H 'Content-Type: application/json' \
  -d '{"path":"/data/ArtemisLM/models/model.gguf"}'
curl http://192.168.1.19:47282/unload -H 'Content-Type: application/json' -d '{}'
```

`/v1/models` is an OpenAI model list. Each entry has `context_length` for the
context this console would use, and an `artemislm` object with the file path,
whether it fits, and whether it is the loaded one. `/load` and `/unload`
restart the app. A model that does not fit is refused. With nothing loaded,
port 47281 still serves the page, `GET /v1/models` is an empty list, and
`GET /health` answers 503 until a model is ready.

## LM Studio

LM Studio does not add the PlayStation as one of its own models. It calls the
console as an agent, through MCP. One model stays on the GPU. Any model name
on the chat port reaches the model that is loaded.

The script below works with a console on 0.10.0. LM Studio launches it.
Program, Install, Edit mcp.json. Use the console's address:

```json
{
  "mcpServers": {
    "artemis-ps5": {
      "command": "node",
      "args": ["D:/path/to/PS5LM-0.9.0/scripts/lmstudio/artemis-mcp.mjs"],
      "env": { "ARTEMIS_HOST": "192.168.1.19" }
    }
  }
}
```

The model in LM Studio then has five tools: `artemis_status`, `artemis_models`,
`artemis_ask`, `artemis_load`, and `artemis_reference`. The reference tool converts
units, does calendar dates, and looks up constants, elements, HTTP statuses, and
short text encodings stored on the console. It needs 0.12.0 or later on the console.
Ask sends the question to port 47281. When
Agent mode on the console is Plan or Hand off, the script writes a short plan
first and then the answer, both on the loaded model. Hand off does not restart
from this script. Load restarts the app onto another file, so the next ask
should wait until status shows that model.

The app also serves MCP on the library port once this source is installed:

```json
{
  "mcpServers": {
    "artemis-ps5": { "url": "http://192.168.1.19:47282/mcp" }
  }
}
```

`POST /v1/agent` on that same port takes `prompt`, `system`, `messages`,
`max_tokens`, and `plan`, and returns an OpenAI chat completion plus an
`artemis` object with the plan. `GET /v1/agent` and `GET /artemis` name the
chat address and the MCP address.

To use the PlayStation as the model instead of an agent, point an OpenAI
client at `http://192.168.1.19:47281/v1`. LM Studio's own chat still runs its
local model and reaches the console through the tools above.

## Checked on the console

The task, in a folder holding a two-line `hello.py`: "In hello.py, add a
function add(a, b) that returns a + b. Edit the file; do not create other
files." Qwen3.8-27B UD-IQ2_XXS, 64k context, q8_0 cache, on 2026-10-10.

| Tool | Version | Result |
|---|---|---|
| opencode | 1.18.35 | Edited the file; 208 s |
| codex | 0.161.0 | Edited the file and ran it to check; 12,960 tokens. Warns that it has no metadata for the model |
| Claude Code | 2.1.296 | Edited the file; 613 s, most of it the first 18k-token prompt |

Each tool's own prompt (instructions and tool definitions) is 10k to 20k
tokens, processed at about 30 tokens a second on this model: the first answer
takes minutes, later turns reuse the cache and are faster.

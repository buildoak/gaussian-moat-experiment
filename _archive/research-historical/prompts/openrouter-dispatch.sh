#!/bin/bash
# Dispatch sqrt(40) prediction prompt to OpenRouter models
# Usage: ./openrouter-dispatch.sh <model-slug> [output-dir]
# Example: ./openrouter-dispatch.sh google/gemini-2.5-pro
#          ./openrouter-dispatch.sh deepseek/deepseek-r1
#          ./openrouter-dispatch.sh openai/o3-pro

set -euo pipefail

MODEL="${1:?Usage: $0 <model-slug> [output-dir]}"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
OUTPUT_DIR="${2:-$SCRIPT_DIR/../results}"
PROMPT_FILE="$SCRIPT_DIR/2026-03-19-sqrt40-prediction-prompt.md"

# OpenRouter API key from vault
API_KEY="${OPENROUTER_API_KEY_IMAGES:?Set OPENROUTER_API_KEY_IMAGES or export it}"

mkdir -p "$OUTPUT_DIR"

# Sanitize model name for filename
MODEL_SAFE=$(echo "$MODEL" | tr '/' '-')
TIMESTAMP=$(date +%Y%m%d-%H%M%S)
OUTPUT_FILE="$OUTPUT_DIR/${TIMESTAMP}-${MODEL_SAFE}.json"
TEXT_FILE="$OUTPUT_DIR/${TIMESTAMP}-${MODEL_SAFE}.md"

# Read prompt
PROMPT_CONTENT=$(cat "$PROMPT_FILE")

echo ">>> Dispatching to: $MODEL"
echo ">>> Output: $OUTPUT_FILE"

# Build JSON payload
PAYLOAD=$(jq -n \
  --arg model "$MODEL" \
  --arg content "$PROMPT_CONTENT" \
  '{
    model: $model,
    messages: [
      {
        role: "user",
        content: $content
      }
    ],
    max_tokens: 16384,
    temperature: 0.3
  }')

# Send request
HTTP_CODE=$(curl -s -w "%{http_code}" -o "$OUTPUT_FILE" \
  https://openrouter.ai/api/v1/chat/completions \
  -H "Content-Type: application/json" \
  -H "Authorization: Bearer $API_KEY" \
  -H "HTTP-Referer: https://github.com/gaussian-moat" \
  -H "X-Title: Gaussian Moat Research" \
  -d "$PAYLOAD")

if [ "$HTTP_CODE" -ne 200 ]; then
  echo "!!! HTTP $HTTP_CODE — request failed"
  cat "$OUTPUT_FILE"
  exit 1
fi

# Extract text response
jq -r '.choices[0].message.content // "NO_CONTENT"' "$OUTPUT_FILE" > "$TEXT_FILE"

# Extract usage
PROMPT_TOKENS=$(jq -r '.usage.prompt_tokens // "?"' "$OUTPUT_FILE")
COMPLETION_TOKENS=$(jq -r '.usage.completion_tokens // "?"' "$OUTPUT_FILE")
TOTAL_COST=$(jq -r '.usage.total_cost // "?"' "$OUTPUT_FILE")

echo ">>> Done: $MODEL"
echo "    Prompt tokens: $PROMPT_TOKENS"
echo "    Completion tokens: $COMPLETION_TOKENS"
echo "    Cost: $TOTAL_COST"
echo "    Text: $TEXT_FILE"
echo "    Raw JSON: $OUTPUT_FILE"

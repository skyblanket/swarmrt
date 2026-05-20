# Embed.sw — thin embedding client for OpenAI-compatible APIs.
#
# `Embed.create(opts, text)` POSTs to `<endpoint>/v1/embeddings` and
# returns the embedding vector as a list of floats. opts:
#   %{endpoint: "https://api.openai.com", key: "sk-...", model: "text-embedding-3-small"}
#
# Works against OpenAI, Voyage, Mixedbread, BGE servers, Ollama
# (/v1/embeddings), and any other OpenAI-shaped endpoint.

module Embed
export [create, create_batch]

fun create(opts, text) {
    url = base_url(opts) ++ "/v1/embeddings"
    body = json_encode(%{
        model: map_get(opts, 'model'),
        input: text
    })
    hdrs = [{"Authorization", "Bearer " ++ to_string(map_get(opts, 'key'))},
            {"Content-Type", "application/json"}]
    resp = http_post(url, hdrs, body)
    if (resp == nil) { nil }
    else { extract_one(json_decode(resp)) }
}

fun create_batch(opts, texts) {
    url = base_url(opts) ++ "/v1/embeddings"
    body = json_encode(%{
        model: map_get(opts, 'model'),
        input: texts
    })
    hdrs = [{"Authorization", "Bearer " ++ to_string(map_get(opts, 'key'))},
            {"Content-Type", "application/json"}]
    resp = http_post(url, hdrs, body)
    if (resp == nil) { nil }
    else {
        decoded = json_decode(resp)
        data = map_get(decoded, 'data')
        extract_each(data, [])
    }
}

fun extract_one(decoded) {
    if (decoded == nil) { nil }
    else {
        data = map_get(decoded, 'data')
        if (data == nil || length(data) == 0) { nil }
        else { map_get(hd(data), 'embedding') }
    }
}

fun extract_each(data, acc) {
    if (length(data) == 0) { acc }
    else {
        item = hd(data)
        extract_each(tl(data), list_append(acc, map_get(item, 'embedding')))
    }
}

fun base_url(opts) {
    case map_get(opts, 'endpoint') {
        nil -> "https://api.openai.com"
        url -> to_string(url)
    }
}

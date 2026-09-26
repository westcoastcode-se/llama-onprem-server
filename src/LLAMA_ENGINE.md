# LlamaEngine

Plan för `src/server/llm/llm_engine.cpp`. Varje punkt är det som ska vara sant i koden.

## 1. Jinja-mall

`llama_chat_apply_template` kör inte Jinja. En mall som innehåller `<|im_start|>` blir inbyggd ChatML, så `<think>`, `enable_thinking` och verktyg från `src/templates/` aldrig når modellen.

- Länka `llama-common`.
- Formatera med `common_chat_templates_apply` (`use_jinja = true`). Om mallens parser inte går att bygga används `force_pure_content`, så prompttexten ändå blir Jinja-resultatet.
- `template_path` vinner över mallen som ligger i GGUF. Tom sökväg läser GGUF-mallen.
- `LlamaConfig::reasoning` (ersätter den oanvända `resoning`) sätter `enable_thinking`.
- Verktyg på sessionen skickas in i mallen som OpenAI-formade funktioner (`name`, `description`, `parameters`).
- Ta bort oanvända `src/server/llm/template.hpp`.
- Qwen-mallen ber modellen svara med `<function=…><parameter=…>`. `parse_assistant_actions` ska förstå både det formatet och det befintliga JSON-inuti-`<tool_call>`.

## 2. Prefix i tokens

KV-prefixet får inte mätas som en bytelängd i den omritade mallen. EOG avkodas inte, medan mallen lägger till `<|im_end|>` när assistentturen ritas om. Nästa tur klistrades då direkt på svaret.

- Spara tokenföljden som faktiskt ligger i KV (`active_tokens_`).
- Tokenisera hela den nya prompten med samma flaggor varje gång (`add_special = true`, `parse_special = true`), samma väg som llama.cpp-servern.
- Behåll det gemensamma tokenprefixet, klipp resten med `llama_memory_seq_rm` och avkoda bara skillnaden.
- Avkoda inte EOG. Nästa prompts `<|im_end|>` blir en riktig token i deltat.

Tänkande mallar ritar ofta om assistentturen (`<think>` runt hela innehållet). Då tar prefixet slut där texten slutar stämma, och den turen avkodas igen. Resultatet är rätt. Träffen täcker historiken fram till den omskrivna turen.

## 3. Återställ KV när anropet inte blir en färdig tur

`cached_messages_` uppdaterades bara när `generate()` returnerade, medan KV fylldes token för token. Ett decode-fel lämnade cachen före historiken.

- Vid ingången till ett anrop är checkpoint det gemensamma prefixet efter klippningen.
- Vid decode-fel, för kort kontext eller avbrott: klipp tillbaka till checkpoint innan felet lämnar motorn.
- Kontext som tar slut kastas som `LlamaContextFull`. Jobbet blir `Error` och sessionen sparar inte ett avklippt assistentmeddelande.
- Avbrott (`should_stop` eller token-callback som returnerar false) återställer checkpoint och returnerar texten som hunnit skapas, utan att den blir nästa prefix. `Jobs` markerar `Cancelled`.
- `max_tokens` och EOG är färdiga turer: de genererade tokenarna ligger kvar i KV.

## 4. KV per session

En processgemensam cache gör varje sessionsbyte till en full omprefill.

- `MessagesRequest::session_id` nycklar cachen.
- Den aktiva sessionen ligger i kontextens sekvens 0. Övriga sparas med `llama_state_seq_get_data` / `llama_state_seq_set_data`.
- `LlamaConfig::kv_sessions` (standard 2) är hur många sessioner som behålls, inklusive den som är laddad. Övriga LRU-kastas.
- `Sessions` ber `Jobs` släppa KV när en session tas bort eller GC:as. Släppet körs på worker-tråden, inte under `llama_decode`.
- Ett sparat tillstånd kan vara stort. Håll `kv_sessions` lågt.

## 5. Avbrott och tak

- `should_stop` kollas mellan prompt-batcher, inte bara efter första genererade stycket.
- `max_tokens` finns på `LlamaRequest`, `MessagesRequest`, sessionen och `POST`-kroppen. `-1` betyder ingen gräns. Ett anrop ärver sessionens tak när meddelandet inte sätter ett eget.

## 6. Kontext, sampling, RAII

- `n_threads`, `n_threads_batch` (`0` = llama.cpp-default).
- `flash_attn`: `auto` / `on` / `off`.
- `cache_type_k` och `cache_type_v` (`f16` som default, plus `f32`, `bf16`, `q8_0`, `q4_0`, `q4_1`, `q5_0`, `q5_1`, `iq4_nl`).
- `no_perf = false`, och varje anrop loggar prefill- och decode-tid från `llama_perf_context`.
- `seed` till distributionssamplern. `penalty_last_n` och `frequency_penalty` är konfig, inte konstanter.
- Temperatur per anrop bygger om samplern utan att skriva tillbaka i `LlamaConfig`.
- `create()` kastar och låter destruktorn frigöra modell, kontext och mall.

## CLI

`callisto_server`:

`--chat-template`, `--reasoning` / `--no-reasoning`, `--threads`, `--threads-batch`, `--flash-attn`, `--cache-type-k`, `--cache-type-v`, `--seed`, `--penalty-last-n`, `--frequency-penalty`, `--max-tokens`, `--kv-sessions` (bara callisto).

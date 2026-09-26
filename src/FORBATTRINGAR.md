# Förbättringar

Förslag från code review av `src`. Sessions-id som tidsstämpel (`Session::id`) behålls. Det räcker för syftet just nu.

## Klart

- [x] Reasoning-turen delas. När mallen har öppnat `<think>` i prompten sparas kroppen i `reasoning_content` och bara texten efter `</think>` i `content`. Saknas avslutaren blir hela svaret tanke, och inga verktyg köas. Nästa tur skickar `reasoning_content` till Jinja-mallen.
- [x] Verktygsanrop inuti `<think>`, `<thinking>`, `<thought>` eller `<reasoning>` parsas inte. Klienten får ett `<think>`-prefix på tokenströmmen när reasoning är på, så den befintliga visningen kan dölja tanken.
- [x] Sista NDJSON-raden har `state` och `error`. Klienten kastar när `state` är `error`. Sessionen sparar felet och `GET /v1/sessions/:id` kan visa det.
- [x] Tom assistant-text går att serialisera. En generation som inte skrev något sparas inte som ett meddelande.
- [x] HTTP-felen skickar `e.what()`. Ogiltigt id ger 400 `invalid id` i stället för 500.
- [x] `SIGINT`/`SIGTERM` skriver till ett rör. En vanlig tråd avbryter jobb och anropar `listen`-stop. Workern joinas inte från signalhanteraren. `request_shutdown` väcker tokenströmmar som sitter i `wait_pull`.
- [x] `delete_session` i `defer` sväljer fel, så en död server inte anropar `std::terminate` från en `noexcept`-destruktor.
- [x] Tokenströmmen buffras till hela NDJSON-rader.
- [x] EOF i godkännandefrågan avslutar turen. Den räknas inte som nej som fortsätter agentloopen.
- [x] `-it` under 1 avvisas innan en generation startas. `-it` utan värde ger samma fel som övriga flaggor som saknar värde.
- [x] XML-parametrar sparas som rå text. Ett omgivande radbrytningspar tas bort, övriga blanksteg blir kvar. `true` förblir strängen `true`. Tal, bool, array och objekt tolkas bara när verktygets schema säger det. En JSON-kropp utan `<parameter>`-taggar, och ett `arguments`-fält som är en JSON-sträng, packas upp.
- [x] `tool_parameters` kastar inte när JSON Schema-`type` är en array som `["string","null"]`.
- [x] `make_model_adapter` matchar filnamnet, inte katalogen.
- [x] En systemroll i historiken slås ihop med serverns systemprompt, så mallen inte får två systemmeddelanden.
- [x] `set_cancelled` sätter `finished_at`, så avbrutna jobb kan samlas in.
- [x] `AwaitingTools` och `AwaitingQuestion` får samma idle-timeout som en vanlig vilande session.
- [x] Hjälptexten på servern säger att standardvärdet för `--host` är `127.0.0.1`.
- [x] Klientens hjälptext lovar inte längre `-c`, `-e`, `--command`, `-q`, `--quiet`, `--oneshot` eller positionsargument. `all` och `*` i `--allow-tools` står i hjälpen.
- [x] `web_search` annonserar `categories`, `language` och `time_range`. `web_fetch` annonserar `text` och `html`. `sub_agent` beskriver att `task` också kan vara en array.
- [x] `execute_command` kör i en egen processgrupp, med stdin från `/dev/null` och stderr på samma rör som stdout. Tidsgräns 180 s. Trunkerad utdata dödar gruppen i stället för att vänta i `pclose`. `EINTR` (Ctrl-C) dödar gruppen.
- [x] `write_file` skriver till en temporär fil och byter namn. Ett misslyckat skriv avbryter och lämnar den gamla filen.
- [x] `read_file` svarar `(empty file)` för en tom fil och läser inte en hel rad utan tak in i minnet.
- [x] `search_text` jämför `tolower` på `unsigned char` på båda sidor, så `för` matchar `för`.
- [x] `http_get` begränsar protokollet till http/https, kapar kroppen vid 1 MiB, och avvisar loopback, privata och länklokala adresser efter anslutning. `web_search` får fortfarande prata med lokal SearXNG.
- [x] Trasig eller saknad `</think>` sätter inte `thinking_done_bit`. `tests` avslutar med felkod när ett påstående faller.
- [x] Den inklistrade JSON-kommentaren i godkännandeavsnittet i `src/common/tools.cpp` är borttagen.

## Kvar

- [ ] En `<parameter>`-värde som själv innehåller `</parameter>` klipps fortfarande vid första stängningstaggen. Det behövs ett escape eller en avgränsare som inte kan förekomma i värdet.
- [ ] DeepSeek V4:s DSML-format (`<｜DSML｜invoke>`) har ingen parser. Ett filnamn med `deepseek` väljer fortfarande V3-dialekten.
- [ ] Jinja-mallen och `default_agent_system_prompt` beskriver verktygsformatet båda två. En av dem ska äga instruktionen.
- [ ] Verktygsnamn, beskrivningar och verktygsresultat escapas inte. `<|im_end|>` i en fil som läses in kan avsluta ChatML-turen.
- [ ] Ingen autentisering. Bindning utanför loopback är öppen. Hjälptexten stämmer med loopback-standard, men `--host 0.0.0.0` kräver fortfarande en delad hemlighet om den ska vara säker.
- [ ] Samplern matar inte promptens sista token till penalty-fönstret. `--repetition-penalty` och liknande ser bara token från den pågående genereringen.
- [ ] En prompt som är ett prefix av cachen kan sampla från förra batchens logits. Den tomma svansen ska avkoda sista token igen.
- [ ] En bruten tokenström avbryter inte GPU-jobbet. Bara SIGINT gör `DELETE` på jobbet.
- [ ] `build_system_prompt` i `src/common/tools.cpp` används inte och lär fortfarande ut JSON inuti `<tool_call>`. Ta bort den när inget externt anrop finns kvar.
- [ ] Godkännandet visar sökvägen modellen skickade, inte symlinkens mål. Det finns ingen rot som håller verktygen inne i arbetskatalogen.
- [ ] Kommandot ärver klientens miljö. En egen minimal miljö är kvar.
- [ ] `std::regex` i `search_text` har ingen tidsgräns.
- [ ] Sub-agenten: flera `tasks` ser inte varandras svar, sammanfattningsturen har fortfarande verktyg, och barnprompten nämner `sub_agent` trots att verktyget är bortfiltrerat.
- [ ] En klient som tappar anslutningen mitt i `wait_pull` lämnar generationen igång tills den tar slut. Strömmen avbryter inte jobbet när sinken dör.

## Medvetet oförändrat

- Sessions-id är `high_resolution_clock`. Ingen räknare och ingen kollisionsspärr.

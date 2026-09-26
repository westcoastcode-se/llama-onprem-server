# Engine hardening

Checklist from the LlamaEngine code review. Check items when the code is changed.

- [x] RAII + move-only LlamaEngine
- [x] TokenCallback-dokumentation = kod (`true` = continue)
- [x] temp_override chat→generate; ingen config-mutation
- [x] Fel propageras (inte bara `""`)
- [x] template_path fallback
- [x] NDJSON newlines
- [x] Jobs::stop avbryter generation
- [x] Atomisk/säkrare JobKey
- [x] utf8 invalid tail
- [x] string_view-alias (eller engine utan alias)
- [x] kFinishedTtl flyttad/bort från engine
- [x] Known: KV-cache inte per session

Session-KV, Jinja-mallar och tokenprefix beskrivs i [LLAMA_ENGINE.md](LLAMA_ENGINE.md).

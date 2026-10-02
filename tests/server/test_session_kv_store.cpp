#include "common/defer.hpp"
#include "server/llm/session_kv_store.hpp"
#include "../tests.hpp"

#include "llama.h"

#include <fstream>
#include <unistd.h>

namespace
{

void write_u32(std::ostream &out, uint32_t value)
{
    out.write(reinterpret_cast<const char *>(&value), sizeof(value));
}

// The llama sequence header, plus enough token bytes for token_count to accept it.
// The state body is omitted, so this file must not be passed to llama_state_seq_load_file.
void write_seq_header(const std::filesystem::path &path, uint32_t n_tokens, bool with_token_bytes)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    write_u32(out, LLAMA_STATE_SEQ_MAGIC);
    write_u32(out, LLAMA_STATE_SEQ_VERSION);
    write_u32(out, n_tokens);
    if (with_token_bytes && n_tokens != 0)
    {
        const std::vector<char> tokens(static_cast<size_t>(n_tokens) * sizeof(llama_token), '\0');
        out.write(tokens.data(), static_cast<std::streamsize>(tokens.size()));
    }
}

} // namespace

/**
 * The header reports the token count, a second id can take a copy, and a short or foreign file is rejected.
 * Saving and loading the sequence body needs a llama_context, so those calls stay out of this test.
 */
static int test_session_kv_header()
{
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / ("callisto-kv-" + std::to_string(::getpid()));
    defer(std::filesystem::remove_all(dir));

    SessionKvStore store;
    store.open(dir);

    write_seq_header(dir / "42.kv", 3, true);
    assertEquals(static_cast<size_t>(3), store.token_count("42").value_or(0));

    assertTrue(store.copy("42", "7"));
    assertEquals(static_cast<size_t>(3), store.token_count("7").value_or(0));
    store.remove("42");
    assertTrue(!store.token_count("42").has_value());
    assertEquals(static_cast<size_t>(3), store.token_count("7").value_or(0));

    const std::vector<int32_t> tokens{3, 9, 12};
    assertTrue(!store.save(nullptr, "42", tokens));
    assertTrue(!store.save(nullptr, "../escape", tokens));
    std::vector<int32_t> loaded{1};
    assertTrue(!store.load(nullptr, "7", loaded));
    assertTrue(loaded.empty());

    std::ofstream junk(dir / "9.kv", std::ios::binary | std::ios::trunc);
    junk << "nope";
    junk.close();
    assertTrue(!store.token_count("9").has_value());
    assertTrue(!store.load(nullptr, "9", loaded));

    write_seq_header(dir / "11.kv", 3, false);
    assertTrue(!store.token_count("11").has_value());

    write_seq_header(dir / "13.kv", 2'000'000, false);
    assertTrue(!store.token_count("13").has_value());
    return EXIT_SUCCESS;
}

int test_session_kv_store()
{
    RUN_TEST(test_session_kv_header);
    return EXIT_SUCCESS;
}

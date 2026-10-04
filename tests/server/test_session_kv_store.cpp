#include "common/defer.hpp"
#include "server/llm/session_kv_store.hpp"
#include "server/llm/session_park.hpp"
#include "../tests.hpp"

#include "llama.h"

#include <chrono>
#include <fstream>
#include <iterator>
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
    write_seq_header(dir / "thread_1.kv", 3, true);
    assertEquals(static_cast<size_t>(3), store.token_count("thread_1").value_or(0));
    assertTrue(!store.token_count("..").has_value());
    assertTrue(!store.token_count("../escape").has_value());

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

/**
 * A captured state drops its 8-byte prefix and keeps the llama sequence header.
 */
static int test_save_captured_writes_llama_header()
{
    const auto dir = make_temp_dir("kv-captured");
    defer(std::filesystem::remove_all(dir));
    SessionKvStore store;
    store.open(dir);

    const std::vector<int32_t> tokens{7, 8};
    std::vector<uint8_t> state(kSeqMemoryPrefix + 4, 0);
    state[kSeqMemoryPrefix] = 'B';
    state[kSeqMemoryPrefix + 1] = 'O';
    state[kSeqMemoryPrefix + 2] = 'D';
    state[kSeqMemoryPrefix + 3] = 'Y';
    assertTrue(store.save_captured("thread_1", tokens, state));
    assertEquals(static_cast<size_t>(2), store.token_count("thread_1").value_or(0));

    std::ifstream in(dir / "thread_1.kv", std::ios::binary);
    std::vector<char> file((std::istreambuf_iterator<char>(in)), {});
    const std::size_t header = sizeof(uint32_t) * 3 + tokens.size() * sizeof(int32_t);
    assertEquals(header + 4, file.size());
    assertEquals('B', file[header]);
    assertEquals('Y', file[header + 3]);

    std::vector<uint8_t> short_state(kSeqMemoryPrefix - 1, 0);
    assertTrue(!store.save_captured("thread_1", tokens, short_state));
    assertEquals(static_cast<size_t>(2), store.token_count("thread_1").value_or(0));
    assertTrue(!store.save_captured("../escape", tokens, state));
    return EXIT_SUCCESS;
}

/**
 * RAM keeps a session that fits, spills the oldest first, and refuses one larger than the budget.
 * Replacing an id does not spill that same id.
 */
static int test_session_memory_budget()
{
    SessionSnapshot sample;
    sample.state.assign(40, 1);
    sample.tokens = {1};
    const uint64_t one = parked_session_bytes(sample);
    assertEquals(static_cast<uint64_t>(40 + sizeof(int32_t)), one);

    SessionMemory memory{100};
    memory.insert("a", sample);
    memory.insert("b", sample);
    assertEquals(one * 2, memory.used_bytes());
    assertEquals(static_cast<size_t>(1), memory.token_count("a").value_or(0));

    SessionSnapshot next = sample;
    next.state.assign(30, 2);
    const MemoryAdmit admit = memory.plan(parked_session_bytes(next), "");
    assertTrue(admit.keep_incoming);
    assertEquals(static_cast<size_t>(1), admit.spill.size());
    assertEquals(std::string("a"), admit.spill[0]);

    const auto spilled = memory.take("a");
    assertTrue(spilled.has_value());
    memory.insert("c", std::move(next));
    assertTrue(!memory.contains("a"));
    assertTrue(memory.contains("b"));
    assertTrue(memory.contains("c"));
    const std::vector<std::string> order = memory.ids_oldest_first();
    assertEquals(static_cast<size_t>(2), order.size());
    assertEquals(std::string("b"), order[0]);
    assertEquals(std::string("c"), order[1]);

    SessionSnapshot huge;
    huge.state.assign(200, 3);
    huge.tokens = {1};
    const MemoryAdmit rejected = memory.plan(parked_session_bytes(huge), "");
    assertTrue(!rejected.keep_incoming);
    assertTrue(rejected.spill.empty());

    const MemoryAdmit replaced = memory.plan(one, "b");
    assertTrue(replaced.keep_incoming);
    assertTrue(replaced.spill.empty());

    SessionMemory off{0};
    const MemoryAdmit disabled = off.plan(one, "");
    assertTrue(!disabled.keep_incoming);
    return EXIT_SUCCESS;
}

/**
 * Old files are removed only while the directory is over the byte limit. Newest first stays.
 */
static int test_disk_gc_plan_and_files()
{
    using clock = std::chrono::file_clock;
    const auto now = clock::time_point{} + std::chrono::hours{24 * 100};
    const DiskSessionStat files[] = {
        {"recent", 100, now - std::chrono::hours{24}},
        {"older", 100, now - std::chrono::hours{24 * 30}},
        {"old", 100, now - std::chrono::hours{24 * 10}},
    };
    const DiskGcPlan dropped = plan_session_disk_gc(files, 7, 150, now);
    assertEquals(static_cast<size_t>(2), dropped.drop.size());
    assertEquals(std::string("older"), dropped.drop[0]);
    assertEquals(std::string("old"), dropped.drop[1]);

    const DiskGcPlan under = plan_session_disk_gc(files, 7, 1000, now);
    assertTrue(under.drop.empty());

    const DiskGcPlan off = plan_session_disk_gc(files, -1, 0, now);
    assertTrue(off.drop.empty());

    const auto dir = make_temp_dir("kv-gc");
    defer(std::filesystem::remove_all(dir));
    write_test_file(dir / "older.kv", std::string(100, 'a'));
    write_test_file(dir / "old.kv", std::string(100, 'b'));
    write_test_file(dir / "recent.kv", std::string(100, 'c'));
    write_test_file(dir / "note.txt", std::string(1000, 'd'));
    const auto wall = clock::now();
    std::filesystem::last_write_time(dir / "older.kv", wall - std::chrono::hours{24 * 30});
    std::filesystem::last_write_time(dir / "old.kv", wall - std::chrono::hours{24 * 10});
    std::filesystem::last_write_time(dir / "recent.kv", wall - std::chrono::hours{24});

    SessionKvStore store;
    store.open(dir);
    const std::vector<std::string> removed = store.trim_expired(7, 150);
    assertEquals(static_cast<size_t>(2), removed.size());
    assertEquals(std::string("older"), removed[0]);
    assertEquals(std::string("old"), removed[1]);
    assertTrue(!std::filesystem::exists(dir / "older.kv"));
    assertTrue(!std::filesystem::exists(dir / "old.kv"));
    assertTrue(std::filesystem::exists(dir / "recent.kv"));
    assertTrue(std::filesystem::exists(dir / "note.txt"));
    assertTrue(store.trim_expired(7, 1000).empty());
    return EXIT_SUCCESS;
}

int test_session_kv_store()
{
    RUN_TEST(test_session_kv_header);
    RUN_TEST(test_save_captured_writes_llama_header);
    RUN_TEST(test_session_memory_budget);
    RUN_TEST(test_disk_gc_plan_and_files);
    return EXIT_SUCCESS;
}

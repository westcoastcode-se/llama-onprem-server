//
// Created by per on 8/30/26.
//

extern int test_tools();
extern int test_tool_policy();
extern int test_tool_execute_command();
extern int test_tool_read_file();
extern int test_tool_write_file();
extern int test_tool_list_directory();
extern int test_tool_file_search();
extern int test_tool_search_text();
extern int test_tool_subagent();
extern int test_web_utils();
extern int test_defer();
extern int test_xdg();
extern int test_utf8_stream();
extern int test_token_buffer();
extern int test_models();
extern int test_llama_engine();
extern int test_context_params();
extern int test_session_gc();
extern int test_tool_history();
extern int test_session_kv_store();
extern int test_session_disk();
extern int test_server_options();
extern int test_responses();

int main()
{
    if (const int rc = test_tools())
    {
        return rc;
    }
    if (const int rc = test_tool_policy())
    {
        return rc;
    }
    if (const int rc = test_tool_execute_command())
    {
        return rc;
    }
    if (const int rc = test_tool_read_file())
    {
        return rc;
    }
    if (const int rc = test_tool_write_file())
    {
        return rc;
    }
    if (const int rc = test_tool_list_directory())
    {
        return rc;
    }
    if (const int rc = test_tool_file_search())
    {
        return rc;
    }
    if (const int rc = test_tool_search_text())
    {
        return rc;
    }
    if (const int rc = test_tool_subagent())
    {
        return rc;
    }
    if (const int rc = test_web_utils())
    {
        return rc;
    }
    if (const int rc = test_defer())
    {
        return rc;
    }
    if (const int rc = test_xdg())
    {
        return rc;
    }
    if (const int rc = test_utf8_stream())
    {
        return rc;
    }
    if (const int rc = test_token_buffer())
    {
        return rc;
    }
    if (const int rc = test_models())
    {
        return rc;
    }
    if (const int rc = test_llama_engine())
    {
        return rc;
    }
    if (const int rc = test_context_params())
    {
        return rc;
    }
    if (const int rc = test_session_gc())
    {
        return rc;
    }
    if (const int rc = test_tool_history())
    {
        return rc;
    }
    if (const int rc = test_session_kv_store())
    {
        return rc;
    }
    if (const int rc = test_session_disk())
    {
        return rc;
    }
    if (const int rc = test_server_options())
    {
        return rc;
    }
    if (const int rc = test_responses())
    {
        return rc;
    }
    return 0;
}

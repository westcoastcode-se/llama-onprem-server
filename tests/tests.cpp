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
extern int test_tool_schema();
extern int test_session_store();
extern int test_transcript_scroll();
extern int test_utf8_stream();
extern int test_token_buffer();
extern int test_models();

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
    if (const int rc = test_tool_schema())
    {
        return rc;
    }
    if (const int rc = test_session_store())
    {
        return rc;
    }
    if (const int rc = test_transcript_scroll())
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
    return 0;
}

//
// Created by per on 8/30/26.
//

extern int test_tools();
extern int test_tool_execute_command();

int main()
{
    if (const int rc = test_tools())
    {
        return rc;
    }
    if (const int rc = test_tool_execute_command())
    {
        return rc;
    }
    return 0;
}

---
name: review
description: Review a code change from the diff. Cite file:line findings and do not paste whole files.
---

# Code review

Review the change. The diff is the source. Unchanged files are not.

## What to read

1. For uncommitted work, run `git diff` and `git diff --cached` with `execute_command`. For a branch, run `git diff <base>...HEAD`.
2. `execute_command` keeps about 8000 characters. When the output says it was truncated, review one path at a time with `git diff -- path`. Write that file's findings before opening the next path.
3. Open a source file only when a hunk is not enough to judge a bug. Call `read_file` with `offset` at the hunk and a `limit` of about 40 lines. Do not use the default 500-line window.
4. Do not read `.callisto/map.md`, and do not read another skill, to locate the change. The diff names the files.

## What to write

Write the findings for a file before the next tool call. A newer tool result replaces older file bodies with one line. That line still names the path and the line window, and `read_file` can fetch it again.

Each finding is `path:line` and what is wrong: a bug, a regression, or a missing test. Skip style tours and narration of files you opened.

## Sub-agent

A sub-agent that reviews follows this procedure. Its result is the findings list. It does not paste file contents. The parent keeps that list.

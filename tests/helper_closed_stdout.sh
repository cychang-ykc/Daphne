#!/usr/bin/env bash
# The host closes the helper's stdout, then a command comes (MIKU gone while the helper cleans up): the reply's
# write must fail, not end the helper by SIGPIPE (exit 141) before the cleanup that the end of input runs.
helper="$1"
(sleep 1; echo '{"id":1,"cmd":"hello"}'; sleep 1) | "$helper" | head -c 1 > /dev/null
code=${PIPESTATUS[1]}
echo "daphne-helper exit code: $code"
[ "$code" -eq 0 ]

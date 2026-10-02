# Vulkan CTS runner

`run_cts.py` runs selected cases and flushes each completed result to a CSV journal. `--resume` skips completed entries and runs remaining/new cases. Failures, crashes and timeouts stay recorded; use a new output file if you intend to retry them.

Ctrl-C/SIGTERM terminates and reaps the current deqp process and exits 130. `--stop-on-crash` ends the run after a crash/timeout; it does not reset the GPU or start another batch. Pick paths and case selections for your own test installation with `--help`.

This runner is useful for focused results. A passing selection is not a full CTS result, and a diagnostic timeout is not a completed benchmark.

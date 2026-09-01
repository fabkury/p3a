# CLAUDE.md

@AGENTS.md

## Claude Code notes

Project guidance lives in `AGENTS.md`, imported above. Edit that file when the
repository's conventions change. This section holds only what is specific to
the Claude Code harness.

- The Bash and PowerShell tools share one persistent working directory across
  calls, but shell state (environment variables, the activated IDF profile)
  does not persist. A `cd` in an earlier call silently moves a later `idf.py`
  invocation. When the user asks for a build, do the whole sequence in one
  command: set `PYTHONUTF8`, dot-source the IDF profile, set
  `ESP_IDF_VERSION`, `Set-Location` to the repo root, then `idf.py`. The
  symptom of drift is "Running cmake in directory ...\components\<name>\build"
  and a stray `build/` inside that component.

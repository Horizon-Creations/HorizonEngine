# Handoff template

Copy the block below as the last section of a step's doc. Replace everything in angle
brackets; delete lines that do not apply. Target: at most ~60 lines.

```markdown
## Handoff

- **Topic / step:** <topic number or name>, step <n> of <m>
- **Branch / commit:** <branch> @ <short sha> (pushed: yes/no)
- **Done:** <what now works, in one or two sentences, user-visible terms>
- **Files touched:** <path:line — why>, ... (the ones a reader must open, not every file)
- **Verified:** <what was run and what it showed: command + result, test names, a screenshot path>
- **Not verified:** <what could not be run here and why (no toolchain, no GPU, no server)>
- **Decisions taken:** <decision — reason>
- **Decisions suggested, not taken:** <for the queen / the human>
- **Pending:** <what the topic still needs, in order>
- **Traps:** <things that cost time: a build flag, a file that must not be read whole, a flaky test>
- **Next steps:** 1. <first concrete action, with file/function>  2. ...
```

## Why this exists

A worker that starts without one reads the same big files again, re-derives the same
decisions and spends the same tokens as the step before it. "Verified / Not verified" keeps
the next worker from either trusting a guess or re-proving a fact.

## Hive preamble (paste into the prompt the Hive gives each worker)

The repository's `CLAUDE.md` already contains these rules; the Hive's prompt only needs
to point at them, so they are loaded at the start of every worker:

```
Read CLAUDE.md first (it is short). Then read only the Handoff section of this topic's
latest doc under docs/ and continue from "Next steps"; do not re-survey the repo.
Look for the hive_* tools once; if hive_join fails or the tools are missing, stop at once
and report "Hive unreachable (<error>). Nothing claimed, nothing changed." — no retries,
no repo exploration, no edits. End every step with a Handoff section (docs/handoff-template.md).
```

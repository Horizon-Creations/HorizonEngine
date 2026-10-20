#!/usr/bin/env python3
"""he_taskboard_live — the task board's MCP tools (topic 203) against a live editor.

One hidden HorizonEditor over a private HOME, config and copy of the tutorial
project, driven through the real shim (he_mcp.Bridge). What it checks is the
editor's own wiring, which tests/test_mcp_tools_taskboard.cpp cannot reach: the
tools are registered by EditorApplication::setupMcpTools, their board is the
Task Board panel's (TaskBoardPanel::board/edit), and that board is the project's
Config/TaskBoard.json.

  1. tools/list has task_list, task_create, task_update, task_assign, task_notices.
  2. task_list on the project: no team (`team.connected` false).
  3. task_create → the task is in Config/TaskBoard.json, recorded under `author`.
  4. task_update (status + comment) → status and note in the file.
  5. task_assign → no_team (no team server on this branch), file unchanged.
  6. task_notices → connected false, none.
  7. A pull: a second task written into the file by hand (as git would) shows
     up in task_list, and the next task_create keeps it — the board reloads
     what arrived instead of saving over it.

NOT checked here: anything with a team. This branch has no team transport
(TeamController/TeamClient are on release/0.7.0), so the two-editor notice run
is tests/test_mcp_tools_taskboard.cpp over tests/FakeTaskTeam.h until then.

Usage:
    scripts/he_taskboard_live.py OUTDIR [--editor PATH] [--project DIR] [--timeout S]
"""

import json
import pathlib
import shutil
import sys
import time
import uuid

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import he_mcp  # noqa: E402
from he_mcp_multiclient import PROJECT_SRC, expect, log  # noqa: E402
from he_rewards_live import EDITOR, PROJECT_FALLBACKS, Editor, setup  # noqa: E402

TOOLS = ["task_list", "task_create", "task_update", "task_assign", "task_notices"]


def call(bridge, tool, args):
    """(isError, structuredContent or the error text)."""
    reply = bridge.request("tools/call", {"name": tool, "arguments": args})
    if "error" in reply:
        raise RuntimeError("%s → rpc error %s" % (tool, reply["error"]))
    res = reply["result"]
    if res.get("isError"):
        text = " ".join(c.get("text", "") for c in res.get("content", []))
        return True, text
    return False, res.get("structuredContent") or {}


def board_file(project):
    return project / "Config" / "TaskBoard.json"


def read_board(project):
    with open(board_file(project), encoding="utf-8") as f:
        return json.load(f)


def task_in(board, tid):
    return next((t for t in board.get("tasks", []) if t.get("id") == tid), None)


def run(outdir, editor_path, timeout, failures):
    project = outdir / "project"
    ed = Editor(outdir, "live", editor_path, timeout)
    try:
        ed.wait_endpoint()
        bridge = he_mcp.Bridge(str(ed.endpoint))
        bridge.ensure()

        log("1. tools/list")
        names = {t["name"] for t in bridge.request("tools/list", {})["result"]["tools"]}
        for t in TOOLS:
            expect(t in names, "%s is registered" % t, failures)

        log("2. task_list, no team")
        err, sc = call(bridge, "task_list", {})
        expect(not err, "task_list answers (%s)" % (sc if err else "ok"), failures)
        if err:
            return
        expect(sc["team"]["connected"] is False, "team.connected is false", failures)
        before = len(sc["tasks"])
        author = sc["author"]
        log("  author %r, %d task(s) before" % (author, before))

        log("3. task_create")
        err, sc = call(bridge, "task_create", {"title": "Live: retexture the crates",
                                               "priority": "high"})
        expect(not err, "task_create answers (%s)" % (sc if err else "ok"), failures)
        if err:
            return
        tid = sc["task"]["id"]
        disk = task_in(read_board(project), tid)
        expect(disk is not None, "the task is in Config/TaskBoard.json", failures)
        expect(disk and disk.get("priority") == "high", "priority high on disk", failures)
        expect(disk and disk.get("createdBy") == author, "created by %r on disk" % author, failures)

        log("4. task_update")
        err, sc = call(bridge, "task_update", {"id": tid, "status": "in-progress",
                                               "comment": "Started live"})
        expect(not err and sc["task"]["status"] == "in-progress", "task_update answers in-progress", failures)
        disk = task_in(read_board(project), tid) or {}
        expect(disk.get("status") == "in-progress", "status in-progress on disk", failures)
        last = (disk.get("updates") or [{}])[-1]
        expect(last.get("kind") == "status" and last.get("text") == "Started live",
               "the status update carries its note on disk", failures)

        log("5. task_assign without a team")
        err, text = call(bridge, "task_assign", {"id": tid, "assignees": ["Bob"]})
        expect(err and "no_team" in text, "task_assign refuses with no_team (%s)" % text, failures)
        disk = task_in(read_board(project), tid) or {}
        expect(not disk.get("assignees"), "no assignees on disk", failures)

        log("6. task_notices without a team")
        err, sc = call(bridge, "task_notices", {})
        expect(not err and sc["connected"] is False and sc["notices"] == [],
               "task_notices: not connected, none", failures)

        log("7. a pull under the open board")
        j = read_board(project)
        pulled = uuid.uuid4().hex
        j["tasks"].append({"id": pulled, "title": "Pulled: light the pier", "status": "open",
                           "priority": "normal", "createdBy": "Teammate"})
        # A different size than before, so the editor's mtime+size check sees it
        # even inside one second of the last save.
        with open(board_file(project), "w", encoding="utf-8") as f:
            json.dump(j, f, indent=2)
        seen = False
        for _ in range(40):
            err, sc = call(bridge, "task_list", {})
            if not err and any(t["id"] == pulled for t in sc["tasks"]):
                seen = True
                break
            time.sleep(0.25)
        expect(seen, "task_list shows the pulled task", failures)
        err, sc = call(bridge, "task_create", {"title": "Live: after the pull"})
        expect(not err, "task_create after the pull answers", failures)
        j = read_board(project)
        expect(task_in(j, pulled) is not None, "the pulled task survived the next save", failures)
        expect(task_in(j, tid) is not None, "the first task survived too", failures)
        expect(not err and task_in(j, sc["task"]["id"]) is not None, "the new task is on disk", failures)

        bridge.close()
    finally:
        ed.stop()


def main():
    args = sys.argv[1:]
    if not args:
        print(__doc__)
        return 2
    outdir = pathlib.Path(args[0]).resolve()
    editor_path, project_src, timeout = EDITOR, PROJECT_SRC, 900
    for i, a in enumerate(args):
        if a == "--editor":
            editor_path = pathlib.Path(args[i + 1]).resolve()
        if a == "--project":
            project_src = pathlib.Path(args[i + 1]).resolve()
        if a == "--timeout":
            timeout = int(args[i + 1])
    if not project_src.exists():
        project_src = next((p for p in PROJECT_FALLBACKS if p.exists()), project_src)
    if not editor_path.exists() or not project_src.exists():
        log("missing editor %s or project %s" % (editor_path, project_src))
        return 2
    if outdir.exists():
        shutil.rmtree(outdir)
    outdir.mkdir(parents=True)
    setup(outdir, project_src)

    failures = []
    try:
        run(outdir, editor_path, timeout, failures)
    except Exception as e:  # noqa: BLE001 — the run's result line must still come
        failures.append("exception: %s" % e)
        log("  FAIL exception: %s" % e)
    log("RESULT: %s (%d failures)" % ("PASS" if not failures else "FAIL", len(failures)))
    for f in failures:
        log("  - " + f)
    return 0 if not failures else 1


if __name__ == "__main__":
    sys.exit(main())

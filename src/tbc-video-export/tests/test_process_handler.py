from __future__ import annotations

import json
import logging
import subprocess
from pathlib import Path
from typing import TYPE_CHECKING

from tbc_video_export.process.process_handler import ProcessHandler
from tests.conftest import get_path

if TYPE_CHECKING:
    from collections.abc import Callable

    import pytest
    from pytest_mock import MockFixture

    from tbc_video_export.program_state import ProgramState

PROBE_ANAMORPHIC = {
    "streams": [
        {
            "index": 0,
            "width": 1136,
            "height": 626,
            "sample_aspect_ratio": "5:6",
        }
    ]
}

PROBE_SQUARE = {
    "streams": [
        {
            "index": 0,
            "width": 1136,
            "height": 626,
            "sample_aspect_ratio": "1:1",
        }
    ]
}


def _make_output_file(state: ProgramState) -> Path:
    output_file = state.file_helper.output_video_file
    output_file.write_bytes(b"ORIGINAL")
    return output_file


MKVMERGE_GUI_OUTPUT = [
    "mkvmerge v102.0 ('Little Houses') 64-bit\n",
    "'in.mkv': Using the demultiplexer for the format 'Matroska'.\n",
    "#GUI#progress 0%\n",
    "#GUI#progress 50%\n",
    "#GUI#progress 50%\n",
    "#GUI#progress 100%\n",
    "Multiplexing took 0 seconds.\n",
]


class _FakeMkvmerge:
    """Stand-in for the mkvmerge Popen: writes --output, replays output lines."""

    def __init__(
        self, command: list[str], lines: list[str], returncode: int, writes: bool
    ) -> None:
        self.stdout = iter(lines)
        self.returncode = returncode

        if writes:
            Path(command[command.index("--output") + 1]).write_bytes(b"REMUXED")

    def __enter__(self) -> _FakeMkvmerge:
        return self

    def __exit__(self, *_exc: object) -> None:
        return None


def _patch_process_tools(  # noqa: D103
    mocker: MockFixture,
    probe: dict,
    mkvmerge_available: bool,
    *,
    mkvmerge_lines: list[str] | None = None,
    mkvmerge_returncode: int = 0,
    runtime_file: Path = Path("/nonexistent/bin/tbc-video-export"),
) -> list[list[str]]:
    """Patch ffprobe/mkvmerge subprocesses and return the commands run.

    runtime_file stands in for the launcher's location, which decides where a
    packaged build's bundled mkvmerge would be.
    """
    commands: list[list[str]] = []

    def fake_run(command: list[str], **_kwargs: object):
        commands.append(command)
        return subprocess.CompletedProcess(
            command, 0, stdout=json.dumps(probe), stderr=""
        )

    def fake_popen(command: list[str], **_kwargs: object) -> _FakeMkvmerge:
        commands.append(command)
        return _FakeMkvmerge(
            command,
            MKVMERGE_GUI_OUTPUT if mkvmerge_lines is None else mkvmerge_lines,
            mkvmerge_returncode,
            writes=mkvmerge_returncode in (0, 1),
        )

    which_results = {
        "ffprobe": "/usr/bin/ffprobe",
        "mkvmerge": "/usr/bin/mkvmerge" if mkvmerge_available else None,
    }

    mocker.patch(
        "tbc_video_export.process.process_handler.shutil.which",
        side_effect=which_results.get,
    )
    mocker.patch(
        "tbc_video_export.process.process_handler.subprocess.run",
        side_effect=fake_run,
    )
    mocker.patch(
        "tbc_video_export.process.process_handler.subprocess.Popen",
        side_effect=fake_popen,
    )
    mocker.patch(
        "tbc_video_export.process.process_handler.files.get_runtime_directory",
        return_value=runtime_file,
    )

    return commands


def test_normalize_mkv_display_aspect_anamorphic(  # noqa: D103
    program_state: Callable[[list[str], str, str | None], ProgramState],
    mocker: MockFixture,
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    monkeypatch.chdir(tmp_path)
    state = program_state([], f"{get_path('pal_svideo')}.tbc", "out_file")
    output_file = _make_output_file(state)

    commands = _patch_process_tools(mocker, PROBE_ANAMORPHIC, True)
    ProcessHandler(state)._normalize_mkv_display_aspect()  # noqa: SLF001

    # ffprobe ran, then mkvmerge with pixel-unit display dimensions
    assert len(commands) == 2
    assert commands[1][commands[1].index("--timestamp-scale") + 1] == "1"
    assert commands[1][commands[1].index("--display-dimensions") + 1] == "0:947x626"

    # progress arrives as lines (--gui-mode), never suppressed (--quiet)
    assert "--gui-mode" in commands[1]
    assert "--quiet" not in commands[1]

    # output atomically replaced with the remuxed file, no temp leftovers
    assert output_file.read_bytes() == b"REMUXED"
    assert not list(tmp_path.glob("*.remux-*.mkv"))

    assert any(
        "Normalized MKV display dimensions to 947x626" in message.message
        for message in state.export.messages
    )


def test_normalize_mkv_display_aspect_logs_progress_with_process_output(  # noqa: D103
    program_state: Callable[[list[str], str, str | None], ProgramState],
    mocker: MockFixture,
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
    caplog: pytest.LogCaptureFixture,
) -> None:
    monkeypatch.chdir(tmp_path)
    state = program_state(
        ["--show-process-output"], f"{get_path('pal_svideo')}.tbc", "out_file"
    )
    _make_output_file(state)
    _patch_process_tools(mocker, PROBE_ANAMORPHIC, True)
    # opt validation warns about ANSI support on a non-terminal; not ours
    caplog.clear()

    with caplog.at_level(logging.INFO, logger="console"):
        ProcessHandler(state)._normalize_mkv_display_aspect()  # noqa: SLF001

    # the progress view is off, so a caller reading the output sees the step,
    # each percent once, and the result
    assert [record.getMessage() for record in caplog.records] == [
        "Normalizing MKV display dimensions to 947x626 with mkvmerge",
        "Normalizing MKV display dimensions: 0%",
        "Normalizing MKV display dimensions: 50%",
        "Normalizing MKV display dimensions: 100%",
        "Normalized MKV display dimensions to 947x626 (pixel units).",
    ]


def test_normalize_mkv_display_aspect_reports_mkvmerge_failure(  # noqa: D103
    program_state: Callable[[list[str], str, str | None], ProgramState],
    mocker: MockFixture,
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
    caplog: pytest.LogCaptureFixture,
) -> None:
    monkeypatch.chdir(tmp_path)
    state = program_state([], f"{get_path('pal_svideo')}.tbc", "out_file")
    output_file = _make_output_file(state)
    state.export.messages.clear()

    _patch_process_tools(
        mocker,
        PROBE_ANAMORPHIC,
        True,
        mkvmerge_lines=["#GUI#progress 0%\n", "Error: no space left on device\n"],
        mkvmerge_returncode=2,
    )
    # opt validation warns about ANSI support on a non-terminal; not ours
    caplog.clear()

    with caplog.at_level(logging.ERROR, logger="console"):
        ProcessHandler(state)._normalize_mkv_display_aspect()  # noqa: SLF001

    # original kept, no temp leftovers, the reason travels with the message
    assert output_file.read_bytes() == b"ORIGINAL"
    assert not list(tmp_path.glob("*.remux-*.mkv"))
    assert [record.getMessage() for record in caplog.records] == [
        "mkvmerge failed to normalize MKV display dimensions (exit 2): "
        "Error: no space left on device"
    ]
    assert any("(exit 2)" in message.message for message in state.export.messages)


def test_normalize_mkv_display_aspect_skips_square(  # noqa: D103
    program_state: Callable[[list[str], str, str | None], ProgramState],
    mocker: MockFixture,
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    monkeypatch.chdir(tmp_path)
    state = program_state([], f"{get_path('pal_svideo')}.tbc", "out_file")
    output_file = _make_output_file(state)

    # ProgramState.export is a class-level shared singleton; start clean
    state.export.messages.clear()

    commands = _patch_process_tools(mocker, PROBE_SQUARE, True)
    ProcessHandler(state)._normalize_mkv_display_aspect()  # noqa: SLF001

    # only ffprobe ran, file untouched, no messages
    assert len(commands) == 1
    assert output_file.read_bytes() == b"ORIGINAL"
    assert not state.export.messages


def test_normalize_mkv_display_aspect_warns_without_mkvmerge(  # noqa: D103
    program_state: Callable[[list[str], str, str | None], ProgramState],
    mocker: MockFixture,
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
    caplog: pytest.LogCaptureFixture,
) -> None:
    monkeypatch.chdir(tmp_path)
    state = program_state([], f"{get_path('pal_svideo')}.tbc", "out_file")
    output_file = _make_output_file(state)

    # ProgramState.export is a class-level shared singleton; start clean
    state.export.messages.clear()

    commands = _patch_process_tools(mocker, PROBE_ANAMORPHIC, False)

    with caplog.at_level(logging.ERROR, logger="console"):
        ProcessHandler(state)._normalize_mkv_display_aspect()  # noqa: SLF001

    # only ffprobe ran, file untouched, warning message added and logged (the
    # progress view has already drawn its last frame, so it would never show)
    assert len(commands) == 1
    assert output_file.read_bytes() == b"ORIGINAL"
    assert any(
        "mkvmerge not found" in message.message for message in state.export.messages
    )
    assert any("mkvmerge not found" in record.getMessage() for record in caplog.records)


def test_normalize_mkv_display_aspect_uses_bundled_mkvmerge(  # noqa: D103
    program_state: Callable[[list[str], str, str | None], ProgramState],
    mocker: MockFixture,
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    monkeypatch.chdir(tmp_path)
    state = program_state([], f"{get_path('pal_svideo')}.tbc", "out_file")
    output_file = _make_output_file(state)

    # a packaged build: <prefix>/bin/tbc-video-export, <prefix>/libexec/...
    prefix = tmp_path / "prefix"
    bundled = prefix / "libexec" / "tbc-video-export" / "mkvmerge"
    bundled.parent.mkdir(parents=True)
    bundled.write_text("")

    commands = _patch_process_tools(
        mocker,
        PROBE_ANAMORPHIC,
        False,
        runtime_file=prefix / "bin" / "tbc-video-export",
    )
    ProcessHandler(state)._normalize_mkv_display_aspect()  # noqa: SLF001

    # no mkvmerge on PATH, so the bundled one ran
    assert commands[1][0] == str(bundled)
    assert output_file.read_bytes() == b"REMUXED"


def test_normalize_mkv_display_aspect_skips_non_mkv(  # noqa: D103
    program_state: Callable[[list[str], str, str | None], ProgramState],
    mocker: MockFixture,
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    monkeypatch.chdir(tmp_path)

    # force a mov container
    state = program_state(
        ["--profile", "prores_hq"], f"{get_path('pal_svideo')}.tbc", "out_file"
    )
    commands = _patch_process_tools(mocker, PROBE_ANAMORPHIC, True)

    ProcessHandler(state)._normalize_mkv_display_aspect()  # noqa: SLF001

    assert not commands

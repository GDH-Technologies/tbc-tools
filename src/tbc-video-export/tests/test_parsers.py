from __future__ import annotations

from tbc_video_export.common.enums import ProcessName
from tbc_video_export.process.parser.parser_ld_dropout_correct import (
    ParserLDDropoutCorrect,
)
from tbc_video_export.process.parser.parser_ld_process_vbi import ParserLDProcessVBI


def test_process_vbi_parser_reads_fork_progress_lines() -> None:  # noqa: D103
    parser = ParserLDProcessVBI(ProcessName.LD_PROCESS_VBI)

    parser.parse_line("Info: Using 3 threads to process 391460 fields")
    assert parser.tracked_value_total == 391460

    # this fork's form carries the total as well as the count
    parser.parse_line("Info: Processing fields 1957/391461 (0%, 389504 to go)")
    assert parser.tracked_value == 1957
    assert parser.tracked_value_total == 391461


def test_process_vbi_parser_reads_upstream_progress_lines() -> None:  # noqa: D103
    parser = ParserLDProcessVBI(ProcessName.LD_PROCESS_VBI)

    parser.parse_line("Info: Using 3 threads to process 500 fields")
    parser.parse_line("Info: Processing field 250")

    assert parser.tracked_value == 250
    assert parser.tracked_value_total == 500


def test_process_vbi_parser_reads_completion_fps() -> None:  # noqa: D103
    parser = ParserLDProcessVBI(ProcessName.LD_PROCESS_VBI)

    parser.parse_line(
        "Info: VBI Processing complete - 391461 fields in 1270.5 seconds ( 308.1 FPS )"
    )

    assert parser.tracked_value == 391461
    assert parser.current_fps == 308.1


def test_dropout_correct_parser_reads_completion_fps() -> None:  # noqa: D103
    parser = ParserLDDropoutCorrect(ProcessName.LD_DROPOUT_CORRECT)

    parser.parse_line(
        "Info: Dropout correction complete - 195730 frames in 990.2 seconds "
        "( 197.7 FPS )"
    )

    assert parser.tracked_value == 195730
    assert parser.current_fps == 197.7

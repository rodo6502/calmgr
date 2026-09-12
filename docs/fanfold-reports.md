# Fanfold reports

The application produces two independent human-readable print files. Both use
fixed 82-byte records: 80 printable ASCII columns followed by CR/LF. Every page
contains exactly `LINES_PER_PAGE` records (70 by default), has no form-feed
character, keeps a wrapped appointment block on one page, and leaves the last
record blank for continuous fanfold paper at 10 CPI and 6 LPI.

## CALR01 ISO-week calendar

Service and JSON command: `print-export`  
Configured path: `PRINT_FILE`

CALR01 produces one year plan for each requested year. It prints every ISO week
from 01 through 52 or 53 as a week-number-only calendar. No appointment details
are printed. The remaining printable columns stay blank.

## CALR02 chronological appointment list

Service and JSON command: `print-list-export`  
Configured path: `LIST_PRINT_FILE`

CALR02 prints only active appointments overlapping the requested date range.
Each appointment occupies one streamlined row with ISO week (WK), full start
date, compact date range, client, and appointment/subject. Long client/subject
values are truncated to the fixed column width instead of wrapping. Unused page
lines are blank up to the fixed footer. Internal appointment IDs are not printed.

The TUI command `P` first asks for `C` (week-number-only calendar) or `L`
(streamlined appointment list) and then for the first and last year. The Web
Export view exposes the same selection.
Batch examples are in `examples/print-export-request.json` and
`examples/print-list-export-request.json`.

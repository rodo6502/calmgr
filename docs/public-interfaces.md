# Public COBOL interfaces

All modules use `CALL ... USING` and initialize their output status. Status
constants are in `appointment-status.cpy`.

| Program | Parameters, in order | Responsibility |
|---|---|---|
| `APPOINTMENT-SERVICE` | service request, service result, config | Every public use case and business rule |
| `APPOINTMENT-REPOSITORY` | repository request, appointment record, config | Sole COBOL interface for live SQLite appointment data and transactions |
| `APPOINTMENT-DATES` | operation, date params, status | validate, ISO year/week, add days |
| `APPOINTMENT-RECURRENCE` | date params, status | next W/B/M/Y date with month-end clamp |
| `APPOINTMENT-BACKUP` | operation, config, status, message | SQLite Backup API snapshot and staged restore |
| `APPOINTMENT-AUDIT` | action, ID, source, detail, config, status | append audit entry |
| `APPOINTMENT-EXPORT` | format, service request, result, config | print, CSV and iCalendar output |
| `APPOINTMENT-CSV-READER` | operation, service request, status, message | controlled CSV stream |
| `APPOINTMENT-JSON` | operation, request path, response path, request, result | controlled JSON transport |
| `APPOINTMENT-CONFIG-ADAPTER` | config, status, message | configuration defaults and parsing |

Service cursor operations are `LIST-OPEN`, repeated `LIST-NEXT`, and
`LIST-CLOSE`. Repository cursor operations follow the same ownership pattern;
callers must close a cursor even at end-of-file. Cursors are process-local and
not safe to share between threads in one executable.

The service result always contains a three-digit status, a user-safe message,
a count, an optional appointment, optional user metadata, and optional output path. Status 0 is
success; 10 invalid request; 11 invalid date; 12 not found/end of cursor; 13
duplicate; 14 optimistic concurrency conflict; 15 forbidden; 20 repository;
21 locked; 22 damaged data; 23 I/O; 30 internal.

The fanfold service operations are `PRINT-EXPORT` for the APPT01 ISO-week
calendar and `PRINT-LIST-EXPORT` for the APPT02 chronological appointment
list. Both receive their range in `ASR-FROM-DATE` and `ASR-TO-DATE`; all
repository reads remain owned by `APPOINTMENT-EXPORT`.

The batch-only user commands call the bounded native authentication adapter
from COBOL. They never expose `password_hash`. Credential-bearing requests can
be read from stdin by passing `-` as the request path.

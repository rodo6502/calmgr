# SQLite data format

Release 1 uses one SQLite database, normally `data/appointments.db`. It never
reads or imports DAT files. `PRAGMA user_version` is 3.

`appointments` contains `schema_version`, the text primary key
`appointment_id`, `series_id`, `start_date`, `end_date`, `iso_year`,
`iso_week`, `client`, `subject`, `status`, `created_at`, `updated_at`,
`cancelled_on`, `cancel_note`, `recurrence`, `series_until`, and `revision`.
Indexes cover date range, ISO year/week, series, status, case-insensitive
client, and case-insensitive subject searches. Dates remain fixed-width
`YYYYMMDD` text so COBOL lexical and numeric ordering agree.

`users` contains the integer primary key `user_id`, case-insensitive unique
`username`, encoded `password_hash`, constrained `role`, `active`, timestamps,
and `revision`. `auth_failures` provides bounded login throttling.
`audit_events` records user lifecycle event type, actor, target, and timestamp;
it has no password or hash column.

The native adapter uses parameterized statements, enables foreign keys and WAL,
and sets a finite busy timeout. Schema mismatch, integrity failure, and a
non-SQLite file map to the stable damaged-data status. Constraint, lock, and
other storage errors map to the established duplicate, locked, and repository
codes.

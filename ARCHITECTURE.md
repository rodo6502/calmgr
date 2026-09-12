# Architecture

The CRT TUI, JSON batch program, and Flask web UI are presentation/transport
layers. Appointment business rules, recurrence, validation, audit, imports,
exports, and both fanfold reports remain COBOL services.

`APPOINTMENT-REPOSITORY` is the sole COBOL appointment persistence interface.
It maps its stable ensure/get/put/update/delete/scan and transaction operations
to `src/native/appointment_sqlite.c`. The bounded C adapter owns all SQL,
prepared bindings, SQLite status mapping, schema creation, backup/restore, and
libsodium Argon2id calls. No SQL occurs in TUI, batch, or Python.

The database contains appointments, users, login-throttle state, and secret-free
user audit events. Multi-record mutations enter a nestable `BEGIN IMMEDIATE`
transaction in the service and finish with COMMIT or ROLLBACK. The SQLite
Backup API produces consistent snapshots. Reset and restore stage complete
databases under temporary names and atomically install them.

Flask reads host/port from the shared configuration, validates configuration
before starting, and invokes only the batch executable with a fixed argument
array. Credential requests travel over stdin. Flask owns CSRF and signed session
transport, while the shared batch/native core owns users, password policy,
authentication, authorization, active state, and throttling.

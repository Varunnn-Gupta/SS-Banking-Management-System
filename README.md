# Banking Management System (CS-513 System Software mini project)

Client/server banking system in C: TCP sockets, one pthread per client, a counting
semaphore (100 clients), binary record files, OFD record locking, and a write-ahead
journal for atomic multi-record operations. Four roles: Customer, Employee, Manager, Administrator.

## Requirements
Linux or WSL2, gcc, make. `sudo apt install build-essential libcrypt-dev`

## Build
    make                 # normal build into build/dev/
    make asan            # AddressSanitizer + UBSan build into build/asan/
    make release         # optimised build into build/release/

## Run (everything from the project root, localhost only)
    ./build/dev/server --seed    # once: creates the starter data in data/
    ./build/dev/server           # terminal 1
    ./build/dev/client           # terminal 2, 3, ... (one per user)
    ./build/dev/server --check   # server stopped: verifies balances against the transaction log

To reset: `rm -rf data`, then seed again. Logs go to `logs/server.log`.

## Starter users (all passwords: pass123)
| Username | Role | Notes |
|---|---|---|
| admin1 | Administrator | user id 1 |
| mgr1 | Manager | user id 2 |
| emp1, emp2 | Employee | user ids 3, 4 |
| cust1 to cust4 | Customer | accounts 1 to 4, opening balances Rs 5000 / 2000 / 10000 / 500 |

## Tests
    make MODE=dev test       # race, ledger, loan and user tests (8 threads each)
    make MODE=asan test      # same under AddressSanitizer
    # network tests need a running server and a raised fd limit:
    ulimit -n 4096           # in both terminals
    make MODE=dev nettest    # same-user login race, 100-client semaphore, fuzzing
Crash tests: start the server with `BANK_CRASH=before_commit|after_commit|mid_apply`;
the next journaled operation kills the server at that point, a normal restart recovers.
`BANK_IDLE_SECS=3` shortens the idle-client timeout for testing.

## Source layout
    include/   models.h protocol.h money.h log.h lock.h storage.h journal.h ledger.h
               loans.h auth.h session.h handlers.h conn.h repair.h seed.h
    src/       server.c client.c conn.c session.c auth.c ledger.c loans.c journal.c
               storage.c lock.c repair.c seed.c log.c h_customer.c h_employee.c
               h_manager.c h_admin.c h_loans.c
    tests/     race_test.c ledger_test.c loan_test.c user_test.c net_test.c
    docs/      class and architecture diagrams, DESIGN.md

## Documentation
docs/01_class_data_model.png, 02_class_modules.png, 03_architecture.png,
04_loan_states.png, 05_transfer_sequence.png, and docs/DESIGN.md (every design decision justified).

## Known limits
Single machine (localhost, no TLS); one server process per data directory (enforced by a lock file);
the protocol uses fixed-size structs because client and server run on the same host.
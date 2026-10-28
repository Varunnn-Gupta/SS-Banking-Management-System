# Design Justification

## Core choices
| Area | Choice | Why | Alternative rejected |
|---|---|---|---|
| Concurrency | thread per client | shared state with no IPC setup, cheap to create, simple fd passing | fork: heavier, needs shared memory for sessions |
| Connection cap | counting semaphore = 100, `sem_wait` before `accept` | extra clients wait in the kernel backlog | unbounded threads |
| Storage | binary files, fixed-size records, record N at `N * size` | O(1) seek, per-record byte-range locks, no library hides the locking | SQLite or text files |
| Money | int64 paise, strict string parsing | floats cannot represent decimals exactly | float or double |
| File locking | Linux OFD locks (`F_OFD_SETLKW`), one `Store` of fds per thread | POSIX fcntl locks belong to the process, so two threads of one server would not exclude each other | plain fcntl, or a global mutex |
| Deadlock | fixed order: loan, then accounts in ascending id | no cycle can form | try-lock and back-off |
| Atomicity | write-ahead redo journal of after-images | one transfer or approval changes 3 to 4 records | write in place and hope |
| Durability | `fsync` before the client sees success | survives power loss | write-behind |
| Sessions | exclusive OFD lock on `sessions.lock`, byte = user id | the kernel frees it when a thread or client dies, so no stuck "logged in" flag | `logged_in` field in the record |
| Passwords | salted SHA-512 `crypt_r`, constant-time compare, identical error for bad user and bad password | no plaintext at rest, no username probing | plaintext |
| Authorization | role checked on the server for every request, re-read from disk | a modified client or a mid-session demotion cannot gain access | trust the client menu |

## How each ACID property is met
- **Atomicity:** journal entry = intent, fsync, COMMITTED, fsync (commit point), apply, DONE. Recovery on startup re-applies COMMITTED entries and discards PREPARED ones. After the commit point an I/O failure aborts the process while locks are held, so recovery finishes the work and clients never see a false failure.
- **Consistency:** invariants are checked inside the lock: amount > 0, no overdraft, no self-transfer, accounts active, legal loan transitions. `--check` replays the transaction log and compares it with every balance.
- **Isolation:** per-record exclusive locks, check and write both inside the lock, ordered locking for multi-record operations.
- **Durability:** fsync of journal, then data files, before replying.

## Loan rules
Lifecycle: PENDING, ASSIGNED, then APPROVED or REJECTED (terminal). One open loan per customer.
- Customer deactivated with a loan pending: the loan stays blocked, approval is refused while the account is inactive, the employee may reject, approval works after reactivation.
- Two managers assign at once or two employees decide at once: the loan lock serialises them and the loser gets a clean error. Credit happens exactly once.
- Assigned employee demoted or removed: the loan returns to PENDING. A startup sweep covers a crash between the role write and the release.
- Approval = account credit + transaction record + loan status in one journal entry.

## Other rules
Last administrator cannot be demoted (mutex-protected, race-tested). Customers never change role. Deactivated accounts can log in and view but not transact. A second server on the same data is refused (lock file) so recovery cannot damage a live server. Idle clients are dropped after 600 s so they cannot hold a semaphore slot.

## Likely viva questions
- *Why threads, not fork?* Shared in-process state and low overhead; a thread crash risk is reduced by never trusting client input (fixed-size fields, forced NUL, role checks).
- *Why OFD locks?* Plain fcntl locks are per process, useless between threads.
- *Why fsync?* Without it a success message can be lost in a crash.
- *Crash mid-transfer?* Journal: before the commit point nothing happened, after it recovery completes it.
- *Stuck sessions?* A session is a lock, released by the kernel on death.
- *Why truncate the journal at startup?* Recovery has just resolved every entry, and the instance lock guarantees no other server is using it.
- *How do you prove correctness?* Stress tests (8 threads), invariant checker, ASan, 100-client semaphore test, crash injection.
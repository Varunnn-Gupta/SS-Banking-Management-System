# Banking Management System

**CS-513: System Software Mini Project**

A multithreaded client-server banking application written in C. The project demonstrates TCP socket communication, concurrent client handling, file-based storage, record locking, and crash-safe transactions using a write-ahead journal.

## Features

- **Client-Server Architecture:** Uses TCP sockets for communication between clients and the banking server.
- **Multithreading:** Creates a dedicated POSIX thread (`pthread`) for each connected client.
- **Concurrency Control:** Uses a counting semaphore to limit concurrent clients to 100.
- **Persistent Storage:** Stores banking records in binary files.
- **Record-Level Locking:** Uses Open File Description (OFD) locks to coordinate concurrent access.
- **Crash Recovery:** Implements write-ahead journaling for atomic multi-record operations and recovery after crashes.
- **Role-Based Access:** Supports four roles with distinct permissions: Customer, Employee, Manager, and Administrator.
- **Transaction Verification:** Performs server-side consistency checks against the transaction log.
- **Automated Testing:** Includes unit, concurrency, network, fuzzing, and crash-recovery tests.
- **Multiple Build Configurations:** Supports development, AddressSanitizer/UBSan, and optimized release builds.

## Tech Stack

| Component | Technology |
|---|---|
| Language | C |
| Networking | TCP sockets |
| Concurrency | POSIX threads (`pthread`) |
| Client Limiting | Counting semaphore |
| Storage | Binary record files |
| Synchronization | OFD record locks |
| Transaction Safety | Write-ahead journal |
| Build System | GNU Make |
| Platform | Linux / WSL2 |
| Cryptographic Library | `libcrypt` |

## Prerequisites

The project requires Linux or WSL2 with GCC and Make installed.

Install the required dependencies on Ubuntu or Debian:

```bash
sudo apt update
sudo apt install build-essential libcrypt-dev
```

## Build Instructions

Run all commands from the project root directory.

```bash
# Development build
make

# AddressSanitizer and UndefinedBehaviorSanitizer build
make asan

# Optimized release build
make release
```

Build artifacts are organized by configuration:

```text
build/
├── dev/
├── asan/
└── release/
```

## Getting Started

### 1. Initialize the database

Run the seed command once to create the initial data in `data/`.

```bash
./build/dev/server --seed
```

### 2. Start the server

Open a terminal in the project root and run:

```bash
./build/dev/server
```

The server runs locally and writes its logs to `logs/server.log`.

### 3. Connect a client

Open another terminal and run:

```bash
./build/dev/client
```

Additional clients can be started in separate terminals to simulate multiple users.

### 4. Verify transaction consistency

Stop the server before running the consistency check:

```bash
./build/dev/server --check
```

This checks account balances against the transaction log.

## Starter Credentials

All starter accounts use the password `pass123`.

| Username | Role | User ID | Account / Notes |
|---|---|---:|---|
| `admin1` | Administrator | 1 | Administrative access |
| `mgr1` | Manager | 2 | Management access |
| `emp1` | Employee | 3 | Employee access |
| `emp2` | Employee | 4 | Employee access |
| `cust1` | Customer | N/A | Account 1, ₹5,000 |
| `cust2` | Customer | N/A | Account 2, ₹2,000 |
| `cust3` | Customer | N/A | Account 3, ₹10,000 |
| `cust4` | Customer | N/A | Account 4, ₹500 |

**Note:** These credentials are provided for demonstration and testing purposes. They should not be used in a production environment.

## Testing

### Unit and concurrency tests

Run the test suite in development mode:

```bash
make MODE=dev test
```

Run the same tests with AddressSanitizer and UndefinedBehaviorSanitizer:

```bash
make MODE=asan test
```

The test suite covers race conditions, ledger operations, loans, and user management. Eight threads are used in the relevant concurrency tests.

### Network tests

Network tests require a running server and an increased file descriptor limit.

**Terminal 1: Start the server**

```bash
ulimit -n 4096
./build/dev/server
```

**Terminal 2: Run network tests**

```bash
ulimit -n 4096
make MODE=dev nettest
```

The network tests cover:

- Concurrent login attempts for the same user
- Enforcement of the 100-client semaphore limit
- Protocol fuzzing

### Crash-recovery tests

Crash injection can be used to test journal recovery at different transaction stages.

Start the server with one of the following environment variables:

```bash
# Crash before commit
BANK_CRASH=before_commit ./build/dev/server

# Crash after commit
BANK_CRASH=after_commit ./build/dev/server

# Crash during record application
BANK_CRASH=mid_apply ./build/dev/server
```

Trigger a journaled operation from a client. The server should terminate at the selected crash point.

Restart the server normally:

```bash
./build/dev/server
```

The journal recovery mechanism should restore the interrupted transaction according to its commit state.

### Idle-client timeout

Set the idle-client timeout to three seconds for testing:

```bash
BANK_IDLE_SECS=3 ./build/dev/server
```

## Project Structure

```text
.
├── include/
│   ├── models.h
│   ├── protocol.h
│   ├── money.h
│   ├── log.h
│   ├── lock.h
│   ├── storage.h
│   ├── journal.h
│   ├── ledger.h
│   ├── loans.h
│   ├── auth.h
│   ├── session.h
│   ├── handlers.h
│   ├── conn.h
│   ├── repair.h
│   └── seed.h
│
├── src/
│   ├── server.c
│   ├── client.c
│   ├── conn.c
│   ├── session.c
│   ├── auth.c
│   ├── ledger.c
│   ├── loans.c
│   ├── journal.c
│   ├── storage.c
│   ├── lock.c
│   ├── repair.c
│   ├── seed.c
│   ├── log.c
│   ├── h_customer.c
│   ├── h_employee.c
│   ├── h_manager.c
│   ├── h_admin.c
│   └── h_loans.c
│
├── tests/
│   ├── race_test.c
│   ├── ledger_test.c
│   ├── loan_test.c
│   ├── user_test.c
│   └── net_test.c
│
├── docs/
│   ├── 01_class_data_model.png
│   ├── 02_class_modules.png
│   ├── 03_architecture.png
│   ├── 04_loan_states.png
│   ├── 05_transfer_sequence.png
│   └── DESIGN.md
│
├── data/                 # Runtime banking records
├── logs/                 # Server logs
├── build/                # Build artifacts
├── Makefile
└── README.md
```

## Documentation

The `docs/` directory contains design diagrams and implementation details.

| Document | Description |
|---|---|
| `01_class_data_model.png` | Class and data model |
| `02_class_modules.png` | Module organization and relationships |
| `03_architecture.png` | Overall system architecture |
| `04_loan_states.png` | Loan lifecycle and state transitions |
| `05_transfer_sequence.png` | Transaction and transfer sequence |
| `DESIGN.md` | Design decisions and their justifications |

## Design Highlights

### Concurrent client handling

Each connected client is handled by a separate POSIX thread. A counting semaphore limits the number of concurrent clients to 100.

### Record-level locking

OFD record locks coordinate access to shared records and help prevent conflicting concurrent updates.

### Atomic multi-record operations

The write-ahead journal stores the information needed to recover interrupted multi-record operations. This helps maintain transaction consistency after a server crash.

### Single-server enforcement

A lock file prevents multiple server processes from operating on the same data directory simultaneously.

### Ledger consistency

The server's check mode compares account balances against the transaction log to help identify inconsistencies.

## Known Limitations

- **Localhost only:** The application is designed to run on a single machine.
- **No TLS:** Network communication is not encrypted.
- **Single server per data directory:** Enforced using a lock file.
- **Fixed-size protocol structures:** The client and server must run on the same host and use compatible structure layouts.
- **Demonstration credentials:** Starter passwords are intended for testing and are not suitable for production use.

This project is intended for educational purposes. It demonstrates system software concepts rather than providing production-grade banking security.

## Reset the Database

To remove the generated banking data and start over, stop the server and run:

```bash
rm -rf data
./build/dev/server --seed
```

**Warning:** This command deletes the existing contents of `data/`. Back up any records you want to keep before resetting the database.

## Learning Objectives

This project demonstrates the following concepts:

- TCP client-server programming in C
- POSIX threads and concurrent request handling
- Semaphores and resource limits
- File-based database design
- OFD record locking and synchronization
- Write-ahead logging and crash recovery
- Transaction integrity and ledger verification
- Automated testing of concurrent and networked software

---

**CS-513: System Software Mini Project**

A project exploring concurrency, storage, synchronization, and transaction recovery in C.



# Peer-to-Peer Distributed File Sharing System

## Overview

This project implements a group-based file sharing system using a peer-to-peer architecture. The system consists of two main components:

1. **Trackers**: Keep track of the clients, their shared files, and assist in communication between clients for file sharing.
2. **Clients**: Users can register, create groups, join groups, share files, and download files from peers within their group.

Trackers are synchronized to ensure that all online trackers share the same information. Clients can download files by connecting to peers in the same group and downloading different pieces from different peers in parallel.

### Key Features

- Multi-tracker support with synchronization between trackers
- Parallel downloading with custom piece selection algorithm
- Piecewise SHA1 hash integrity checks for files
- Group-based sharing and file visibility
- Multi-threaded server-client communication

---

## Tracker Setup

### 1. Run Tracker:

To start a tracker, follow these steps:

```bash
cd tracker
g++ tracker.cpp -o tracker -pthread
./tracker <TRACKER INFO FILE> <TRACKER NUMBER>
```

- `<TRACKER INFO FILE>`: Path to the tracker info file (contains IP and port details of all trackers).
- `<TRACKER NUMBER>`: The tracker number that this instance should use.

Example:

```bash
./tracker tracker_info.txt 1
```

This will start the tracker and synchronize with other trackers (if online).

### 2. Close Tracker:

To gracefully shut down the tracker:

```bash
quit
```

---

## Client Setup

### 1. Run Client:

To start the client:
g++ client.cpp -o client -I$(brew --prefix openssl)/include -L$(brew --prefix openssl)/lib -lssl -lcrypto -lpthread
```bash
cd client
g++ client.cpp -o client -lssl -lcrypto -pthread
./client <IP>:<PORT> <TRACKER INFO FILE>
```

- `<IP>:<PORT>`: The client's IP and port.
- `<TRACKER INFO FILE>`: Path to the tracker info file (contains IP and port details of all trackers).

Example:

```bash
./client 127.0.0.1:18000 tracker_info.txt
```

### 2. Create User Account:

Register a new user with the tracker:

```bash
create_user <user_id> <password>
```

### 3. Login:

Login with an existing user account:

```bash
login <user_id> <password>
```

### 4. Create Group:

Create a new group:

```bash
create_group <group_id>
```

### 5. Join Group:

Request to join an existing group:

```bash
join_group <group_id>
```

### 6. Leave Group:

Leave a group you are part of:

```bash
leave_group <group_id>
```

### 7. List Pending Group Join Requests:

View join requests for a group (if you are the owner):

```bash
list_requests <group_id>
```

### 8. Accept Group Joining Request:

Approve a user's request to join your group:

```bash
accept_request <group_id> <user_id>
```

### 9. List All Groups in the Network:

View all groups in the network:

```bash
list_groups
```

### 10. List All Shareable Files in a Group:

View all files shared by members in a group:

```bash
list_files <group_id>
```

### 11. Upload File:

Share a file within a group:

```bash
upload_file <file_path> <group_id>
```

### 12. Download File:

Download a file from peers in the group:

```bash
download_file <group_id> <file_name> <destination_path>
```

### 13. Show Downloads:

Check the status of your downloads:

```bash
show_downloads
```

The output format will show:

- `[D] [grp_id] filename`: Download in progress.
- `[C] [grp_id] filename`: Download complete.

### 14. Stop Sharing File:

Stop sharing a specific file:

```bash
stop_share <group_id> <file_name>
```

### 15. Logout:

Logout from the system and stop sharing files:

```bash
logout
```

---

## Assumptions

1. **Multi-tracker System**: The system assumes at least one tracker will always be online. Clients will attempt to connect to an available tracker.
2. **SHA1 Hash Integrity**: Each file is divided into 512KB pieces, and the SHA1 hash is computed for each piece. This ensures file integrity during downloads.
3. **Group Ownership**: The creator of a group is its owner, and ownership is transferred if the owner leaves the group.
4. **Piece Selection Algorithm**: The client downloads file pieces from multiple peers concurrently using a custom piece selection algorithm to ensure efficient downloading.

---

## Features Implemented

1. **Tracker Synchronization**: All online trackers stay in sync with each other.
2. **Group-Based Sharing**: Files are shared and visible only within groups.
3. **Parallel Downloading**: Files are downloaded in parallel, with pieces fetched from multiple peers simultaneously.
4. **Peer-to-Peer Communication**: Direct communication between peers for file sharing.
5. **User Account System**: Clients need to create an account and login to participate in the network.
6. **Piecewise SHA1 Hashing**: Ensures file integrity for all downloads.

---

## How the System Works

1. Clients register with a tracker and create groups.
2. Files are shared within groups and visible to other group members.
3. Clients can request to download a file, and the tracker will provide peer information for downloading different pieces of the file.
4. The client downloads the file from multiple peers simultaneously, verifying the integrity of each piece using SHA1 hashes.
5. Downloaded files are made available to other peers immediately.
6. Trackers synchronize shared file and group information between each other to ensure availability.

---

## Conclusion

This system provides a robust peer-to-peer file sharing solution with support for multiple trackers, parallel downloads, and secure file integrity verification.

#include <iostream>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <string>
#include <vector>
#include <cstring>
#include <sstream>
#include <sys/stat.h>
#include <openssl/evp.h> // For SHA1
#include <iomanip>       // For hex formatting
#include <thread>
#include <map>
#include <mutex>
#include <set>
#include <atomic>
#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <openssl/evp.h>
#include <random>
using namespace std;

const size_t PIECE_SIZE = 512 * 1024; // 512KB per piece
map<pair<string, string>, char> fileStatus;
//      grp_id   filename
bool file_exists(const std::string &filename)
{
    struct stat buffer;
    return (stat(filename.c_str(), &buffer) == 0); // returns true if file exists
}

void write_log_file(const std::string &ip, const std::string &port, const std::string &content)
{
    std::string filename = ip + "_" + port + ".txt";
    int flags;

    // Check if the file exists
    if (file_exists(filename))
    {
        // If file exists, append content
        flags = O_WRONLY | O_APPEND;
    }
    else
    {
        // If file doesn't exist, create and truncate (first time only)
        flags = O_WRONLY | O_CREAT | O_TRUNC;
    }

    // Open the file with the appropriate flags
    int fd = open(filename.c_str(), flags, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
    if (fd == -1)
    {
        // Handle error, return if file could not be opened
        return;
    }

    // Write content to the file
    write(fd, content.c_str(), content.length());

    // Close the file descriptor
    close(fd);
}
string computeSHA1(const unsigned char *data, size_t size)
{
    stringstream ss;
    for (int i = 0; i < size; ++i)
    {
        ss << hex << setw(2) << setfill('0') << (int)data[i];
    }
    return ss.str();
}

vector<string> tokenize2(const string &line, char delimiter = ' ')
{
    vector<string> tokens;
    stringstream ss(line);
    string token;
    while (getline(ss, token, delimiter))
    {
        tokens.push_back(token);
    }
    return tokens;
}

atomic<bool> running(true); // To control server shutdown
void *listenForQuit(void *)
{
    std::string input;
    while (running)
    {
        std::cin >> input;
        if (input == "quit")
        {
            running = false;
            std::cout << "Shutting down ..." << std::endl;
            exit(0);
            break;
        }
    }
    return NULL;
}
void handlePeerConnections(int port)
{
    string ip = "127.0.0.1";
    int peer_listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (peer_listen_fd < 0)
    {
        write_log_file(ip, to_string(port), "Socket creation failed\n");
        return;
    }

    sockaddr_in peer_addr;
    peer_addr.sin_family = AF_INET;
    peer_addr.sin_port = htons(port);
    peer_addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(peer_listen_fd, (struct sockaddr *)&peer_addr, sizeof(peer_addr)) < 0)
    {
        write_log_file(ip, to_string(port), "Failed to bind peer listening socket.\n");
        close(peer_listen_fd);
        exit(EXIT_FAILURE);
    }

    if (listen(peer_listen_fd, 3) < 0)
    {
        write_log_file(ip, to_string(port), "Listen failed\n");
        return;
    }

    while (true)
    {
        write_log_file(ip, to_string(port), "Waiting for connections...\n");
        int new_socket;
        sockaddr_in client_address;
        int addrlen = sizeof(client_address);
        if ((new_socket = accept(peer_listen_fd, (struct sockaddr *)&client_address, (socklen_t *)&addrlen)) < 0)
        {
            write_log_file(ip, to_string(port), "Accept failed\n");
            continue;
        }

        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_address.sin_addr, client_ip, sizeof(client_ip));
        int client_port = ntohs(client_address.sin_port);
        write_log_file(ip, to_string(port), "Client connected: " + string(client_ip) + ":" + to_string(client_port) + "\n");

        char buffer[100000] = {0};
        int bytesRead = read(new_socket, buffer, sizeof(buffer) - 1);
        if (bytesRead <= 0)
        {
            write_log_file(ip, to_string(port), "Failed to read from socket.\n");
            close(new_socket);
            continue;
        }
        buffer[bytesRead] = '\0';

        string request(buffer);
        vector<string> tokens = tokenize2(request);

        if (tokens.size() != 3 || tokens[0] != "GET_PIECE")
        {
            write_log_file(ip, to_string(port), "Invalid request format: " + request + "\n");
            close(new_socket);
            continue;
        }

        int pieceIndex = stoi(tokens[1]);
        string filepath = tokens[2];

        // Open the file to read the piece data
        int file_fd = open(filepath.c_str(), O_RDONLY);
        if (file_fd < 0)
        {
            write_log_file(ip, to_string(port), "Failed to open file: " + filepath + "\n");
            close(new_socket);
            continue;
        }

        // Get file size
        struct stat file_stat;
        if (fstat(file_fd, &file_stat) < 0)
        {
            write_log_file(ip, to_string(port), "Failed to get file size\n");
            close(file_fd);
            close(new_socket);
            continue;
        }
        off_t file_size = file_stat.st_size;

        // Calculate piece size and offset
        const size_t PIECE_SIZE = 512 * 1024; // 512 KB
        off_t offset = pieceIndex * PIECE_SIZE;
        size_t pieceSize = min(PIECE_SIZE, static_cast<size_t>(file_size - offset));

        // Check if the piece index is valid
        if (offset >= file_size)
        {
            write_log_file(ip, to_string(port), "Invalid piece index: " + to_string(pieceIndex) + "\n");
            close(file_fd);
            close(new_socket);
            continue;
        }

        // Seek to the correct position in the file for the requested piece
        if (lseek(file_fd, offset, SEEK_SET) < 0)
        {
            write_log_file(ip, to_string(port), "Failed to seek to offset: " + to_string(offset) + "\n");
            close(file_fd);
            close(new_socket);
            continue;
        }

        // Read the piece data
        vector<char> pieceData(pieceSize);
        ssize_t bytesReadPiece = read(file_fd, pieceData.data(), pieceSize);
        if (bytesReadPiece < 0)
        {
            write_log_file(ip, to_string(port), "Failed to read piece data from file.\n");
        }
        else
        {
            pieceSize = bytesReadPiece;
            // Send the piece size first
            uint32_t pieceSizeNetwork = htonl(pieceSize);
            send(new_socket, &pieceSizeNetwork, sizeof(pieceSizeNetwork), 0);

            // Send the requested piece data back to the peer
            send(new_socket, pieceData.data(), pieceSize, 0);
        }

        close(file_fd);
        close(new_socket);
    }
}

bool downloadPiece(const string &ip, int port, int pieceIndex, vector<char> &pieceData, const string &filepath)
{
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0)
    {
        write_log_file(ip, to_string(port), "Socket creation error\n");
        return false;
    }

    sockaddr_in serv_addr;
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(port);

    if (inet_pton(AF_INET, ip.c_str(), &serv_addr.sin_addr) <= 0)
    {
        write_log_file(ip, to_string(port), "Invalid address/ Address not supported\n");
        return false;
    }

    if (connect(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0)
    {
        write_log_file(ip, to_string(port), "Connection Failed\n");
        return false;
    }

    string request = "GET_PIECE " + to_string(pieceIndex) + " " + filepath;
    send(sock, request.c_str(), request.length(), 0);

    // Receive piece size
    uint32_t pieceSizeNetwork;
    if (read(sock, &pieceSizeNetwork, sizeof(pieceSizeNetwork)) != sizeof(pieceSizeNetwork))
    {
        write_log_file(ip, to_string(port), "Failed to receive piece size\n");
        close(sock);
        return false;
    }
    uint32_t pieceSize = ntohl(pieceSizeNetwork);

    // Receive piece data
    pieceData.resize(pieceSize);
    size_t totalReceived = 0;
    while (totalReceived < pieceSize)
    {
        int bytesReceived = read(sock, pieceData.data() + totalReceived, pieceSize - totalReceived);
        if (bytesReceived <= 0)
        {
            write_log_file(ip, to_string(port), "Failed to receive piece data\n");
            close(sock);
            return false;
        }
        totalReceived += bytesReceived;
    }

    close(sock);
    return true;
}
vector<string> tokenize(string buffer)
{
    vector<string> result;
    string current_token;

    for (int i = 0; buffer[i] != '\0'; i++)
    {
        if (buffer[i] == ' ')
        {
            if (!current_token.empty())
            {
                result.push_back(current_token); // Add the token to the vector
                current_token.clear();           // Clear the token for the next word
            }
        }
        else
        {
            current_token += buffer[i]; // Add the character to the current token
        }
    }

    // Add the last token if it's not empty
    if (!current_token.empty())
    {
        result.push_back(current_token);
    }

    return result;
}

string computeHash(const vector<char> &data)
{
    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned int hashLen;

    EVP_MD_CTX *mdctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(mdctx, EVP_sha1(), NULL);
    EVP_DigestUpdate(mdctx, data.data(), data.size());
    EVP_DigestFinal_ex(mdctx, hash, &hashLen);
    EVP_MD_CTX_free(mdctx);

    stringstream ss;
    for (unsigned int i = 0; i < hashLen; i++)
    {
        ss << hex << setw(2) << setfill('0') << (int)hash[i];
    }
    return ss.str();
}

void downloadFile(const string &groupId, const string &fileName, const string &destinationPath, int client_sock, int portthisclientlisteningto, string &ipthisclientlisteningto)
{
    pair<string, string> p;
    p = {fileName, groupId};
    fileStatus[p] = 'D';
    set<int> downloadedPieces;
    string destFilePath = destinationPath + "/" + fileName;
    int destFd = open(destFilePath.c_str(), O_WRONLY | O_CREAT, 0666);
    if (destFd < 0)
    {
        write_log_file(ipthisclientlisteningto, to_string(portthisclientlisteningto), "Failed to open destination file: " + destFilePath + "\n");
        return;
    }

    string request = "get_file_info " + groupId + " " + fileName;
    send(client_sock, request.c_str(), request.length(), 0);

    char buffer[100000] = {0};
    int bytesReceived = read(client_sock, buffer, sizeof(buffer));
    if (bytesReceived <= 0)
    {
        write_log_file(ipthisclientlisteningto, to_string(portthisclientlisteningto), "Failed to receive file info from tracker\n");
        close(destFd);
        return;
    }
    buffer[bytesReceived] = '\0';
    vector<string> res = tokenize2(buffer, '\n');
    if (res.empty())
    {
        write_log_file(ipthisclientlisteningto, to_string(portthisclientlisteningto), "Invalid response from tracker\n");
        close(destFd);
        return;
    }
    vector<string> tokens = tokenize2(res[0], ' ');
    int totalpiecestodownload = stoi(tokens[2]);
    bool done = true;
    int curr = 0;
    int lastpiecedownloaded = -1;
    while (curr < totalpiecestodownload)
    {
        string request = "get_file_info " + groupId + " " + fileName;
        send(client_sock, request.c_str(), request.length(), 0);

        char buffer[100000] = {0};
        int bytesReceived = read(client_sock, buffer, sizeof(buffer));
        if (bytesReceived <= 0)
        {
            write_log_file(ipthisclientlisteningto, to_string(portthisclientlisteningto), "Failed to receive file info from tracker\n");
            close(destFd);
            return;
        }
        buffer[bytesReceived] = '\0';

        map<pair<int, string>, set<pair<int, string>>> pieceMap;
        vector<string> res = tokenize2(buffer, '\n');
        if (res.empty())
        {
            write_log_file(ipthisclientlisteningto, to_string(portthisclientlisteningto), "Invalid response from tracker\n");
            close(destFd);
            return;
        }

        vector<string> tokens = tokenize2(res[0], ' ');
        string filepath = tokens.size() > 1 ? tokens[1] : "";

        for (size_t i = 1; i < res.size(); ++i)
        {
            vector<string> tokens = tokenize2(res[i], ' ');
            if (tokens.size() >= 4)
            {
                int pieceIndex = stoi(tokens[0]);
                string pieceHash = tokens[1];
                set<pair<int, string>> owners;
                for (size_t j = 2; j + 1 < tokens.size(); j += 2)
                {
                    int port = stoi(tokens[j]);
                    string ip = tokens[j + 1];
                    owners.insert({port, ip});
                }
                pieceMap[{pieceIndex, pieceHash}] = owners;
            }
        }

        if (pieceMap.empty())
        {
            break;
        }

        auto it = min_element(pieceMap.begin(), pieceMap.end(),
                              [&downloadedPieces](const auto &a, const auto &b)
                              {
                                  if (downloadedPieces.count(a.first.first))
                                      return false;
                                  if (downloadedPieces.count(b.first.first))
                                      return true;
                                  return a.second.size() < b.second.size();
                              });

        if (it == pieceMap.end() || downloadedPieces.count(it->first.first))
        {
            break;
        }

        int pieceIndex = it->first.first;
        string expectedPieceHash = it->first.second;
        auto owners = it->second;
        pair<int, string> selectedOwner = *owners.begin();
        int port = selectedOwner.first;
        string ip = selectedOwner.second;

        // cout << "Downloading piece " << pieceIndex << " from ip " << ip << " on port " << port << endl;
        if ((int)owners.size() == 0)
        {
            break;
        }
        vector<char> pieceData;
        int rndm = rand() % (int)owners.size();
        auto ownerIt = owners.begin();
        advance(ownerIt, rndm);
        for (; ownerIt != owners.end();)
        {
            pair<int, string> selectedOwner = *ownerIt;
            port = selectedOwner.first;
            ip = selectedOwner.second;
            pieceIndex = it->first.first;
            expectedPieceHash = it->first.second;
            write_log_file(ipthisclientlisteningto, to_string(portthisclientlisteningto), "Attempting to download piece " + to_string(pieceIndex) + " from IP " + ip + " on port " + to_string(port) + "\n");
            pieceData.clear();
            if (downloadPiece(ip, port, pieceIndex, pieceData, filepath))
            {
                write_log_file(ipthisclientlisteningto, to_string(portthisclientlisteningto), "Successfully downloaded piece " + to_string(pieceIndex) + " from IP " + ip + " on port " + to_string(port) + "\n");
                break;
            }
            else
            {
                // Move to the next owner in a circular manner
                ownerIt++;
                if (ownerIt == owners.end())
                {
                    // If we've reached the end of the owners, start from the beginning
                    ownerIt = owners.begin();
                }
            }
        }

        // Compute hash of the received piece
        string computedPieceHash = computeHash(pieceData);

        // Verify the integrity of the piece
        if (computedPieceHash != expectedPieceHash)
        {
            write_log_file(ipthisclientlisteningto, to_string(portthisclientlisteningto), "Integrity check failed for piece " + to_string(pieceIndex) + ". Expected hash: " + expectedPieceHash + ", Computed hash: " + computedPieceHash + "\n");
            continue;
        }
        lastpiecedownloaded = curr;
        curr++;
        off_t offset = static_cast<off_t>(pieceIndex) * PIECE_SIZE;
        if (lseek(destFd, offset, SEEK_SET) < 0)
        {
            write_log_file(ipthisclientlisteningto, to_string(portthisclientlisteningto), "Failed to seek to correct position in file.\n");
            close(destFd);
            return;
        }

        ssize_t bytesWritten = write(destFd, pieceData.data(), pieceData.size());
        if (bytesWritten < 0 || static_cast<size_t>(bytesWritten) != pieceData.size())
        {
            write_log_file(ipthisclientlisteningto, to_string(portthisclientlisteningto), "Failed to write piece data to file.\n");
            close(destFd);
            return;
        }

        write_log_file(ipthisclientlisteningto, to_string(portthisclientlisteningto), "Successfully Verified Piece " + to_string(pieceIndex) + "\n");
        downloadedPieces.insert(pieceIndex);

        string updateMsg = "update_piece_info " + expectedPieceHash + " " + to_string(portthisclientlisteningto) + " " + ip;
        send(client_sock, updateMsg.c_str(), updateMsg.length(), 0);
    }
    close(destFd);

    if (curr != totalpiecestodownload)
    {
        write_log_file(ipthisclientlisteningto, to_string(portthisclientlisteningto), "unable to download from piece" + to_string(lastpiecedownloaded + 1) + "\n");
        done = false;
    }

    if (done)
    {
        cout << "File download completed." << endl;
        fileStatus[p] = 'C';
    }
    else
    {
        cout << "Unable to download file. check logs for more info." << endl;
    }
}
vector<pair<string, int>> readTrackerInfo(const char *filename)
{
    int fd = open(filename, O_RDONLY); // Open file for reading
    if (fd < 0)
    {
        cerr << "Failed to open tracker info file." << endl;
        exit(EXIT_FAILURE);
    }

    const int BUFFER_SIZE = 1024;
    char buffer[BUFFER_SIZE];
    vector<pair<string, int>> trackers;

    // Read file content
    ssize_t bytesRead;
    while ((bytesRead = read(fd, buffer, sizeof(buffer) - 1)) > 0)
    {
        buffer[bytesRead] = '\0'; // Null-terminate the buffer
        stringstream ss(buffer);
        string line;

        // Process each line to extract IP and port
        while (getline(ss, line))
        {
            size_t delimiter = line.find(':');
            if (delimiter != string::npos)
            {
                string ip = line.substr(0, delimiter);
                int port = stoi(line.substr(delimiter + 1));
                trackers.push_back({ip, port});
            }
        }
    }

    close(fd); // Close the file descriptor
    return trackers;
}

// Establish connection to the specified IP and port
int connectToTracker(const string &ip, int port)
{
    // creating a client socket at specified IP and port and then connecting it to tracker
    int client_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (client_sock < 0)
    {
        cerr << "Failed to create socket." << endl;
        return -1;
    }
    int opt = 1;
    // port reusable
    if (setsockopt(client_sock, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &opt, sizeof(opt)))
    {
        std::cerr << "setsockopt failed." << std::endl;
        return -1;
    }

    // Now connect to the tracker
    sockaddr_in tracker_addr;
    tracker_addr.sin_family = AF_INET;
    tracker_addr.sin_port = htons(port); // trakcer port

    if (inet_pton(AF_INET, ip.c_str(), &tracker_addr.sin_addr) <= 0)
    {
        cerr << "Invalid IP address: " << ip << endl;
        close(client_sock);
        return -1;
    }

    if (connect(client_sock, (struct sockaddr *)&tracker_addr, sizeof(tracker_addr)) < 0)
    {
        cerr << "Failed to connect to tracker." << endl;
        close(client_sock);
        return -1;
    }

    return client_sock;
}

// Function to communicate with the tracker
void communicateWithTracker(int client_sock, int port, string &ip)
{
    string input;
    char buffer[100000] = {0};

    while (true)
    {
        cout << "Enter command: ";
        getline(cin, input);
        vector<string> line = tokenize(input);
        if (input == "quit")
        {
            break;
        }
        else if (line[0] == "upload_file")
        {
            if (line.size() != 3)
            {
                cout << "Incorrect number of arguments. Usage: upload_file <filepath> <group_id>\n";
            }
            else
            {
                string filePath = line[1]; // Get the file path from the command line input
                string groupId = line[2];  // Get the group ID from the command line input

                int fileDescriptor = open(filePath.c_str(), O_RDONLY);
                if (fileDescriptor < 0)
                {
                    cout << "Failed to open file: " << filePath << endl;
                    continue;
                }

                const size_t pieceSize = 512 * 1024; // 512 KB pieces
                vector<string> pieceWiseHashes;      // Vector for piece-wise hashes
                unsigned char buffer[pieceSize];     // Buffer for reading pieces

                // Full file hash context using EVP
                EVP_MD_CTX *mdCtx = EVP_MD_CTX_new();          // Create a new EVP context
                EVP_DigestInit_ex(mdCtx, EVP_sha1(), nullptr); // Initialize SHA1 digest for full file hash

                ssize_t bytesRead;
                while ((bytesRead = read(fileDescriptor, buffer, pieceSize)) > 0)
                {
                    // Compute piece-wise hash
                    unsigned char pieceHash[EVP_MAX_MD_SIZE]; // Buffer for piece hash
                    unsigned int pieceHashLength;

                    EVP_Digest(buffer, bytesRead, pieceHash, &pieceHashLength, EVP_sha1(), nullptr); // Compute piece-wise hash

                    // Convert piece hash to string and store it
                    pieceWiseHashes.push_back(computeSHA1(pieceHash, pieceHashLength));

                    // Update full file hash with the current piece
                    EVP_DigestUpdate(mdCtx, buffer, bytesRead);
                }

                // Finalize the full file hash
                unsigned char fullFileHash[EVP_MAX_MD_SIZE]; // Buffer for full file hash
                unsigned int fullFileHashLength;

                EVP_DigestFinal_ex(mdCtx, fullFileHash, &fullFileHashLength); // Finalize the digest

                // Convert full file hash to string
                string fullFileHashStr = computeSHA1(fullFileHash, fullFileHashLength);

                // Free the EVP context
                EVP_MD_CTX_free(mdCtx);

                // Close the file descriptor
                close(fileDescriptor);

                string message = "upload_file " + filePath + " " + groupId + " " + fullFileHashStr + " " + to_string(port) + " " + ip;

                // Add each piece-wise hash
                for (const auto &pieceHash : pieceWiseHashes)
                {
                    message += " " + pieceHash;
                }

                // Send the message to the tracker
                send(client_sock, message.c_str(), message.size(), 0);
                int bytes_received = read(client_sock, buffer, sizeof(buffer) - 1);
                if (bytes_received > 0)
                {
                    buffer[bytes_received] = '\0'; // Null-terminate the response
                    cout << "Tracker Response: " << buffer;
                }
                else
                {
                    cerr << "Failed to receive response from tracker." << endl;
                    return;
                }
            }
        }
        else if (line[0] == "download_file")
        {
            if (line.size() != 4)
            {
                cout << "Incorrect number of arguments. Usage: download_file <group_id> <file_name> <destination_path>\n";
            }
            else
            {
                string groupId = line[1];
                string fileName = line[2];
                string destinationPath = line[3];
                downloadFile(groupId, fileName, destinationPath, client_sock, port, ip);
            }
        }
        else if (line[0] == "show_downloads")
        {
            for (auto it : fileStatus)
            {
                string filename = it.first.first;
                string groupid = it.first.second;
                char status = it.second;
                cout << "[" << status << "] " << "[" << groupid << "] " << filename << endl;
            }
        }
        else if (line[0] == "stop_share")
        {
            if (line.size() != 3)
            {
                cout << "Incorrect number of arguments. Usage: stop_share <group_id> <file_name>\n";
            }
            else
            {
                string input = "";
                for (auto i : line)
                {
                    input += i;
                    input += " ";
                }
                input += ip;
                input += " ";
                input += to_string(port);
                send(client_sock, input.c_str(), input.size(), 0);
                int bytes_received = read(client_sock, buffer, sizeof(buffer) - 1);
                if (bytes_received > 0)
                {
                    buffer[bytes_received] = '\0'; // Null-terminate the response
                    cout << "Tracker Response: " << buffer;
                }
                else
                {
                    cerr << "Failed to receive response from tracker." << endl;
                    return;
                }
            }
        }
        else if (line[0] == "logout")
        {
            string input = "logout ";
            input += ip;
            input += " ";
            input += to_string(port);
            send(client_sock, input.c_str(), input.size(), 0);
            int bytes_received = read(client_sock, buffer, sizeof(buffer) - 1);
            if (bytes_received > 0)
            {
                buffer[bytes_received] = '\0'; // Null-terminate the response
                cout << "Tracker Response: " << buffer;
            }
            else
            {
                cerr << "Failed to receive response from tracker." << endl;
                return;
            }
        }
        else if (line[0] == "login")
        {
            if (line.size() != 3)
            {
                cout << "Incorrect number of arguments passed. login <user_id> <passwd>\n";
            }
            else
            {
                string input = line[0];
                input += " ";
                input += line[1];
                input += " ";
                input += line[2];
                input += " ";
                input += ip;
                input += " ";
                input += to_string(port);
                send(client_sock, input.c_str(), input.size(), 0);
                int bytes_received = read(client_sock, buffer, sizeof(buffer) - 1);
                if (bytes_received > 0)
                {
                    buffer[bytes_received] = '\0'; // Null-terminate the response
                    cout << "Tracker Response: " << buffer;
                }
                else
                {
                    cerr << "Failed to receive response from tracker." << endl;
                    return;
                }
            }
        }
        else if (line[0] == "leave_group")
        {
            if (line.size() != 2)
            {
                cerr << "Incorrect number of arguments passed. leave_group <group_id>\n";
            }
            else
            {
                string input = line[0];
                input += " ";
                input += line[1];
                input += " ";
                input += ip;
                input += " ";
                input += to_string(port);
                send(client_sock, input.c_str(), input.size(), 0);
                int bytes_received = read(client_sock, buffer, sizeof(buffer) - 1);
                if (bytes_received > 0)
                {
                    buffer[bytes_received] = '\0'; // Null-terminate the response
                    cout << "Tracker Response: " << buffer;
                }
                else
                {
                    cerr << "Failed to receive response from tracker." << endl;
                    return;
                }
            }
        }
        else if (line[0] == "join_group")
        {
            if (line.size() != 2)
            {
                cerr << "Incorrect number of arguments passed. join_group <group_id>\n";
            }
            else
            {
                string input = line[0];
                input += " ";
                input += line[1];
                input += " ";
                input += ip;
                input += " ";
                input += to_string(port);
                send(client_sock, input.c_str(), input.size(), 0);
                int bytes_received = read(client_sock, buffer, sizeof(buffer) - 1);
                if (bytes_received > 0)
                {
                    buffer[bytes_received] = '\0'; // Null-terminate the response
                    cout << "Tracker Response: " << buffer;
                }
                else
                {
                    cerr << "Failed to receive response from tracker." << endl;
                    return;
                }
            }
        }
        else
        {
            send(client_sock, input.c_str(), input.size(), 0);
            int bytes_received = read(client_sock, buffer, sizeof(buffer) - 1);
            if (bytes_received > 0)
            {
                buffer[bytes_received] = '\0'; // Null-terminate the response
                cout << "Tracker Response: " << buffer;
            }
            else
            {
                cerr << "Failed to receive response from tracker." << endl;
                return;
            }
        }
    }
}

int main(int argc, char *argv[])
{
    if (argc != 3)
    {
        cerr << "Usage: ./client <IP>:<PORT> tracker_info.txt" << endl;
        return EXIT_FAILURE;
    }

    // Parse IP and port from the command-line argument
    string ip_port = argv[1];
    size_t colon_pos = ip_port.find(':');
    if (colon_pos == string::npos)
    {
        cerr << "Invalid format for IP:PORT" << endl;
        return EXIT_FAILURE;
    }

    string ip = ip_port.substr(0, colon_pos);
    int port = stoi(ip_port.substr(colon_pos + 1));

    // Read tracker information from the specified file
    vector<pair<string, int>> trackers = readTrackerInfo(argv[2]);
    // for (auto it : trackers)
    // {
    //     cout << it.first << ' ' << it.second << endl;
    // }
    // Connect to the tracker
    thread peerThread;
    peerThread = thread(handlePeerConnections, port);
    while (true)
    {
        int client_sock;
        for (const auto &tracker : trackers)
        {
            client_sock = connectToTracker(tracker.first, tracker.second);
            if (client_sock >= 0)
            {
                cout << "Successfully connected to tracker at " << tracker.first << ' ' << tracker.second << endl;
                break;
            }
        }

        if (client_sock < 0)
        {
            cerr << "Failed to connect to any tracker." << endl;
            return EXIT_FAILURE;
        }
        // Communicate with the tracker
        communicateWithTracker(client_sock, port, ip);
        // Close the connection
        cerr << "Lost connection to tracker. Searching for another tracker...Renter the command once connected" << endl;
        close(client_sock); // Close the old socket
        client_sock = -1;
    }
    peerThread.join();

    return 0;
}
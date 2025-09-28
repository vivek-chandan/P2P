#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <iostream>
#include <stdbool.h>
#include <limits.h>
#include <pthread.h>
#include <map>
#include <vector>
#include <fcntl.h>
#include <string>
#include <sstream>
#include <atomic>
#include <unordered_map>
#include <algorithm>
#include <unordered_set>
#include <set>
#include <sys/stat.h>

using namespace std;

atomic<bool> running(true); // To control server shutdown

unordered_map<string, string> user_db;                           // user_id -> password
unordered_map<string, vector<string>> group_db;                  // group_id -> list of users
unordered_map<string, string> logged_in_users;                   // user_id -> client socket
unordered_map<int, string> from_current_client_session;          // port -> currently loggend in user_Id
unordered_map<string, string> group_owner;                       // group , owner
unordered_map<string, vector<string>> groupRequests;             // group_id -> {u2, u3 request}
unordered_map<string, string> full_file_hashes;                  // filename -> fullfilehash
unordered_map<string, unordered_set<string>> group_shared_files; // filename -> {p0hash, p1hash, ...}
unordered_map<string, string> pathoffile;                        // filename -> filepath

//            filename            pieceno.,hashofthatchunk
unordered_map<string, vector<pair<int, string>>> piecewise_hashes;

unordered_map<string, std::set<pair<int, string>>> this_piece_availabale_at;
// piecehases => {8000, a}
// 6289142edd536f3ca2c6c179c4d8c5a1b33036f5 => 8000 a
// c16ae4a94434bda2e51390997c9fb39e6bbaf1f2 => 8000 a

map<pair<int, string>, set<string>> piecesAvailablePreviouslyat;

struct ClientInfo
{
    int client_socket;
    struct sockaddr_in client_address;
    std::string trackerip;
    std::string trackerport;
    std::string othertrackerip;
    std::string othertrackerport;
};
vector<string> tokenizeonspaces(string buffer)
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

// Function to log out a user
string logout(const string &user_id)
{
    if (logged_in_users.find(user_id) != logged_in_users.end())
    {
        logged_in_users.erase(user_id); // Remove the user from logged-in users
        return "Logout successful.\n";
    }
    return "User not logged in.\n";
}
void *listenForQuit(void *)
{
    std::string input;
    while (running)
    {
        std::cin >> input;
        if (input == "quit")
        {
            running = false;
            std::cout << "Shutting down tracker..." << std::endl;
            exit(0);
            break;
        }
    }
    return NULL;
}

int createServerSocket(const std::string &ip, int port)
{
    int server_fd;
    struct sockaddr_in address;

    // Creating socket
    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd == 0)
    {
        std::cerr << "Socket creation failed." << std::endl;
        exit(EXIT_FAILURE);
    }
    // Set socket options to reuse the port and address
    int opt = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &opt, sizeof(opt)))
    {
        std::cerr << "setsockopt failed." << std::endl;
        exit(EXIT_FAILURE);
    }
    // Setting up the address structure
    address.sin_family = AF_INET;
    address.sin_port = htons(port);

    // Convert IP from string to binary form
    if (inet_pton(AF_INET, ip.c_str(), &address.sin_addr) <= 0)
    {
        std::cerr << "Invalid IP address: " << ip << std::endl;
        exit(EXIT_FAILURE);
    }

    // Binding the socket to the specified IP and port
    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0)
    {
        std::cerr << "Binding failed." << std::endl;
        exit(EXIT_FAILURE);
    }

    // Start listening for connections
    if (listen(server_fd, 10) < 0)
    {
        std::cerr << "Listening failed." << std::endl;
        exit(EXIT_FAILURE);
    }

    std::cout << "Tracker is running on " << ip << ":" << port << std::endl;
    return server_fd;
}

std::pair<std::string, int> getTrackerInfo(const char *filename, int tracker_no)
{
    int fd = open(filename, O_RDONLY); // Open file in read-only mode
    if (fd < 0)
    {
        std::cerr << "Unable to open tracker info file." << std::endl;
        exit(EXIT_FAILURE);
    }

    const size_t BUFFER_SIZE = 1024;
    char buffer[BUFFER_SIZE];
    memset(buffer, 0, BUFFER_SIZE); // Initialize buffer

    ssize_t bytesRead;
    int current_line = 1;

    // Read the file line by line
    while ((bytesRead = read(fd, buffer, BUFFER_SIZE - 1)) > 0)
    {
        buffer[bytesRead] = '\0'; // Null-terminate the buffer

        std::istringstream iss(buffer);
        std::string line;

        // Process each line in the buffer
        while (std::getline(iss, line))
        {
            if (current_line == tracker_no)
            {
                // Found the requested line, now parse ip:port
                size_t delimiter_pos = line.find(':');
                if (delimiter_pos != std::string::npos)
                {
                    std::string ip = line.substr(0, delimiter_pos);
                    int port = std::stoi(line.substr(delimiter_pos + 1));
                    close(fd);
                    return {ip, port}; // Return IP and port
                }
            }
            current_line++;
        }
    }

    close(fd); // Close the file descriptor
    std::cerr << "Tracker number not found." << std::endl;
    exit(EXIT_FAILURE);
}
// Function to create a new user account
string create_user(const string &user_id, const string &passwd)
{
    if (user_db.find(user_id) != user_db.end())
    {
        return "User already exists.\n";
    }
    user_db[user_id] = passwd;
    return "User created successfully.\n";
}

// Function to log in a user
string login(const string &user_id, const string &passwd)
{
    if (user_db.find(user_id) == user_db.end())
    {
        return "User not found.\n";
    }
    if (user_db[user_id] != passwd)
    {
        return "Invalid password.\n";
    }
    return "Login successful.\n";
}

// Function to list all groups
string list_groups()
{
    stringstream ss;
    for (const auto &group : group_db)
    {
        ss << group.first << "\n";
    }
    return ss.str();
}

// Function to list requests (simplified version)
string list_requests(const string &group_id)
{
    stringstream ss;
    for (auto user : groupRequests[group_id])
    {
        ss << user << "\n";
    }
    return ss.str();
}

// Function to accept join request (simplified)
string accept_request(const string &group_id, const string &user_id)
{
    group_db[group_id].push_back(user_id); // Add user to the group
    auto &vec = groupRequests[group_id];   // Get a reference to the vector //remove from pending request
    vec.erase(std::remove(vec.begin(), vec.end(), user_id), vec.end());
    return "User added to group.\n";
}
vector<string> tokenize(const char *buffer)
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

string synchronizeTracker(const std::string &logEntry)
{
    vector<string> temp = tokenizeonspaces(logEntry);

    int client_port = stoi(temp.back());
    temp.pop_back();
    string client_ip = temp.back();
    temp.pop_back();
    int client_socket = stoi(temp.back());
    temp.pop_back();

    vector<string> line;
    for (int i = 0; i < (int)temp.size(); i++)
    {
        line.push_back(temp[i]);
    }
    std::string response;
    if (line[0] == "create_user")
    {
        if (line.size() != 3)
        {
            response = "Incorrect number of arguments passed. create_user <user_id> <passwd>\n";
        }
        else
        {
            response = create_user(line[1], line[2]);
        }
    }
    else if (line[0] == "login")
    {
        if (from_current_client_session.find(client_port) != from_current_client_session.end())
        {
            // means somebody is already logged in from current session
            response = "Only one user can be active at once from same port.\n";
        }
        else
        {
            string user_id = line[1];
            string passwd = line[2];
            string ip = line[3];
            int port = stoi(line[4]);
            response = login(user_id, passwd);
            pair<int, string> pp = make_pair(port, ip);
            if (response == "Login successful.\n")
            {
                logged_in_users[user_id] = to_string(client_socket); // Mark user as logged i
                from_current_client_session[client_port] = user_id;
            }
            auto &st = piecesAvailablePreviouslyat[pp];
            for (auto x : st)
            {
                this_piece_availabale_at[x].insert({port, ip});
            }
        }
    }
    else if (line[0] == "logout")
    {
        if (from_current_client_session.find(client_port) == from_current_client_session.end())
        {
            response = "No user logged in.\n";
        }
        else
        {
            string current_user = from_current_client_session[client_port];
            response = logout(current_user);
            from_current_client_session.erase(client_port);
            string ip = line[1];
            int port = stoi(line[2]);
            pair<int, string> pp = make_pair(port, ip);
            // unordered_map<string, vector<string>> group_db; //group_id -> list of users
            for (const auto &x : group_db)
            {
                string groupId = x.first;
                auto &listofusers = x.second;
                if (find(listofusers.begin(), listofusers.end(), current_user) != listofusers.end())
                {

                    //  handling pieces permissions
                    //  unordered_map<string, unordered_set<string>> group_shared_files; // filename -> {file1.txt, file2.txt, ...}
                    const auto &filenames = group_shared_files[groupId];
                    for (const auto &filename : filenames)
                    {
                        const auto &pieceHashes = piecewise_hashes[filename];
                        for (const auto &piece : pieceHashes) // Loop over each piece
                        {
                            // unordered_map<string, std::set<pair<int, string>>> this_piece_availabale_at;
                            string currpiecehashcode = piece.second; // Extract piece hash
                            auto &st = this_piece_availabale_at[currpiecehashcode];
                            piecesAvailablePreviouslyat[pp].insert(currpiecehashcode);
                            st.erase(st.find({port, ip}));
                        }
                    }
                }
            }
            response += "\n";
            response += current_user + " logged out\n";
        }
    }
    else if (line[0] == "create_group")
    {
        if (line.size() != 2)
        {
            response = "Incorrect number of arguments passed. create_group <group_id>\n";
        }
        else if (from_current_client_session.find(client_port) == from_current_client_session.end())
        {
            response = "No user is logged in.\n";
        }
        else
        {
            if (group_db.find(line[1]) != group_db.end())
            {
                response = "Group already exists.\n";
            }
            else
            {
                group_db[line[1]].push_back(from_current_client_session[client_port]); // Initialize the group with an empty user list
                group_owner[line[1]] = from_current_client_session[client_port];
                response = "Group created successfully.\n";
            }
        }
    }
    else if (line[0] == "join_group")
    {
        string ip = line[2];
        int port = stoi(line[3]);
        pair<int, string> pp = make_pair(port, ip);
        if (from_current_client_session.find(client_port) == from_current_client_session.end())
        {
            response = "No user is logged in.\n";
        }
        else
        {
            string group_id = line[1];
            if (group_db.find(group_id) == group_db.end())
            {
                response = "Group not found.\n";
            }
            else
            {
                string user_id = from_current_client_session[client_port];
                groupRequests[group_id].push_back(user_id);
                response = "Request Sent.\n";
            }

            // if this user was a old user of this group and rejoining it again
            // makes him owner of old files that he owned at that time
            auto &st = piecesAvailablePreviouslyat[pp];
            for (auto x : st)
            {
                this_piece_availabale_at[x].insert({port, ip});
            }
        }
    }
    else if (line[0] == "leave_group")
    {
        string group_id = line[1];
        string ip = line[2];
        int port = stoi(line[3]);
        pair<int, string> pp = make_pair(port, ip);
        if (from_current_client_session.find(client_port) == from_current_client_session.end())
        {
            response = "No user is logged in.\n";
        }
        else if (group_db.find(group_id) == group_db.end())
        {
            response = "Group not found.\n";
        }
        else
        {
            string user_id = from_current_client_session[client_port];
            auto &users = group_db[group_id];

            // Find the user in the group
            auto it = find(users.begin(), users.end(), user_id);

            if (it != users.end()) // If the user is found
            {
                // removing permission of download from current ip port
                const auto &filenames = group_shared_files[group_id];
                for (const auto &filename : filenames)
                {
                    const auto &pieceHashes = piecewise_hashes[filename];
                    for (const auto &piece : pieceHashes) // Loop over each piece
                    {
                        // unordered_map<string, std::set<pair<int, string>>> this_piece_availabale_at;
                        string currpiecehashcode = piece.second; // Extract piece hash
                        auto &st = this_piece_availabale_at[currpiecehashcode];
                        piecesAvailablePreviouslyat[pp].insert(currpiecehashcode);
                        st.erase(st.find({port, ip}));
                    }
                }
                // Erase the user from the group
                users.erase(it);
                if (users.empty())
                {
                    group_db.erase(group_id);
                    response = "No members left in the group. Group deleted..\n";
                }
                else if (user_id == group_owner[group_id])
                {
                    group_owner[group_id] = group_db[group_id].back(); // Assigning admin position to the last joined member
                    string temp = "Admin left the group. New Admin is ";
                    temp += group_owner[group_id];
                    response = temp;
                }
                else
                {
                    response = "User removed from group.\n";
                }
            }
            else
            {
                response = "User not found in group.\n";
            }
        }
    }
    else if (line[0] == "list_groups")
    {
        response = list_groups();
    }
    else if (line[0] == "list_requests")
    {
        if (line.size() != 2)
        {
            response = "Incorrect number of arguments passed. list_requests <group_id>\n";
        }
        else if (from_current_client_session.find(client_port) == from_current_client_session.end())
        {
            response = "No user is logged in.\n";
        }
        else
        {
            string group_id = line[1];
            if (group_db.find(group_id) == group_db.end())
            {
                response = "Group not found.\n";
            }
            else if (group_owner[group_id] != from_current_client_session[client_port])
            {
                response = "You are not the owner of this group.\n";
            }
            else
            {
                response = list_requests(line[1]);
            }
        }
    }
    else if (line[0] == "accept_request")
    {
        if (line.size() != 3)
        {
            response = "Incorrect number of arguments passed.accept_request <group_id> <user_id>.\n";
        }
        else if (from_current_client_session.find(client_port) == from_current_client_session.end())
        {
            response = "No user is logged in.\n";
        }
        else
        {
            string group_id = line[1];
            string user_id = line[2];
            if (group_db.find(group_id) == group_db.end())
            {
                response = "Group not found.\n";
            }
            else if (group_owner[group_id] != from_current_client_session[client_port])
            {
                response = "You are not the owner of this group.\n";
            }
            else
            {
                response = accept_request(line[1], line[2]);
            }
        }
    }
    else if (line[0] == "upload_file")
    {
        string filePath = line[1];
        string temp = "";
        size_t lastSlashPos = line[1].find_last_of("/");
        if (lastSlashPos == string::npos)
        {
            temp += "./";
            temp += line[1];
            filePath = temp;
        }
        // Extract just the file name from the full path
        string fileName = (lastSlashPos == string::npos) ? line[1] : line[1].substr(lastSlashPos + 1);
        string groupId = line[2];      // Extract group ID
        string fullFileHash = line[3]; // Extract full file hash
        string user_id = from_current_client_session[client_port];
        pathoffile[fileName] = filePath;
        auto &users = group_db[groupId];
        int availatport = stoi(line[4]);
        string ip = line[5];
        // Find the user in the group
        auto it = find(users.begin(), users.end(), user_id);
        if (group_db.find(groupId) == group_db.end())
        {
            response = "Group not found\n";
        }
        else if (from_current_client_session.find(client_port) == from_current_client_session.end())
        {
            response = "No user is logged in.\n";
        }
        else if (it == users.end())
        {
            response = "You are not a member of this group.\n";
        }
        else
        {
            // Extract piecewise hashes from the remaining tokens
            vector<pair<int, string>> piecewiseHashes;
            for (size_t i = 6; i < line.size(); ++i)
            {
                this_piece_availabale_at[line[i]].insert({availatport, ip});
                piecewise_hashes[fileName].push_back({i - 6, line[i]});
            }
            // Store the full file hash and piecewise hashes in the respective maps
            full_file_hashes[fileName] = fullFileHash;
            group_shared_files[groupId].insert(fileName); // Add the file to the group's shared files
            // Acknowledge the upload to the client
            response = "File " + fileName + " successfully uploaded to group " + groupId + "\n";
        }
    }
    else if (line[0] == "list_files")
    {
        if (line.size() != 2)
        {
            response = "Incorrect number of arguments passed. list_files <group_id>.\n";
        }
        else
        {
            string temp = "";
            string group_id = line[1];
            string user_id = from_current_client_session[client_port];
            auto &users = group_db[group_id];
            // Find the user in the group
            auto it = find(users.begin(), users.end(), user_id);
            if (group_db.find(group_id) == group_db.end())
            {
                response = "Group not found\n";
            }
            else if (from_current_client_session.find(client_port) == from_current_client_session.end())
            {
                response = "No user is logged in.\n";
            }
            else if (it == users.end())
            {
                response = "You are not a member of this group.\n";
            }
            else
            {
                for (auto it : group_shared_files[group_id])
                {
                    temp += it;
                    temp += " ";
                }
                response = temp + "\n";
            }
        }
    }
    else if (line[0] == "get_file_info")
    {
        if (line.size() != 3)
        {
            response = "Error: Invalid number of arguments for get_file_info";
        }
        else
        {
            string groupId = line[1];
            string fileName = line[2];
            string filepath = pathoffile[fileName];
            const auto &it = piecewise_hashes[fileName];
            string totalpieces = to_string((int)it.size());
            if (group_db.find(groupId) == group_db.end())
            {
                response = "Error: Group not found";
            }
            else if (group_shared_files[groupId].find(fileName) == group_shared_files[groupId].end())
            {
                response = "Error: File not found in the group";
            }
            else
            {
                response = "File_Info " + filepath + " " + totalpieces + "\n";
                const auto &pieceHashes = piecewise_hashes[fileName]; // Get the vector of {piece_number, piece_hash}

                for (const auto &piece : pieceHashes) // Loop over each piece
                {
                    int pieceNo = piece.first;               // Extract piece number
                    string currpiecehashcode = piece.second; // Extract piece hash

                    response += to_string(pieceNo) + " " + currpiecehashcode + " "; // Add piece info to response

                    // Now, iterate over peers that have this piece hash
                    const auto &peersWithPiece = this_piece_availabale_at[currpiecehashcode];
                    for (const auto &peer : peersWithPiece) // peer is of type pair<int, string>
                    {
                        response += to_string(peer.first) + " " + peer.second + " "; // Append peer info
                    }
                    response += "\n";
                }
                response += "\n";
            }
        }
    }
    else if (line[0] == "update_piece_info")
    {
        string pieceHash = line[1];
        string availatport = line[2];
        string ip = line[3];
        this_piece_availabale_at[pieceHash].insert({stoi(availatport), ip});
    }
    else if (line[0] == "stop_share")
    {
        // ab jab tak logout karke login nahi karegaa
        // un hashpieces par hold nahi milegi is ip port
        string groupid = line[1];
        string filename = line[2];
        string ip = line[3];
        int port = stoi(line[4]);
        auto it = group_shared_files.find(groupid);
        if (it != group_shared_files.end()) // groupId found
        {
            // Now check if the filename exists in the group's unordered_set
            const unordered_set<string> &sharedFiles = it->second;

            if (sharedFiles.find(filename) == sharedFiles.end()) // filename exists in the set
            {
                response = "File '" + filename + "' does not exist in group '" + groupid + "'.\n";
            }
            else
            {
                // unordered_map<string, vector<pair<int, string>>> piecewise_hashes;
                const auto &pieceHashes = piecewise_hashes[filename];
                for (const auto &piece : pieceHashes) // Loop over each piece
                {
                    // unordered_map<string, std::set<pair<int, string>>> this_piece_availabale_at;
                    string currpiecehashcode = piece.second; // Extract piece hash
                    auto &st = this_piece_availabale_at[currpiecehashcode];
                    pair<int, string> pp = make_pair(port, ip);
                    piecesAvailablePreviouslyat[pp].insert(currpiecehashcode);
                    st.erase(st.find({port, ip}));
                }
                response = "Sharing of file " + filename + " stopped.\n";
            }
        }
        else
        {
            response = "Group '" + groupid + "' does not exist.\n";
        }
    }
    else
    {
        response = "Unknown command.\n";
    }
    return response;
}

void handleClient(int client_socket, struct sockaddr_in client_address, string &trackerip, string &trackerport, string &othertrackerip, string &othertrackerport)
{
    // You can now use both the socket and the address of the client here
    char client_ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &client_address.sin_addr, client_ip, sizeof(client_ip)); // Convert IP to string
    int client_port = ntohs(client_address.sin_port);                           // Get port number
    // cout << "inside handle client" << client_ip << " :" << client_port << endl;
    std::string currentLogFile = trackerip + "_" + trackerport + ".txt";

    char buffer[100000];
    while (true)
    {
        // Open the log file for reading
        int logFileFd = open(currentLogFile.c_str(), O_RDONLY);
        if (logFileFd > 0)
        {
            const int BUFFER_SIZE = 1024;
            char buffer[BUFFER_SIZE];
            std::string logEntry;
            ssize_t bytesRead;
            while ((bytesRead = read(logFileFd, buffer, BUFFER_SIZE)) > 0)
            {
                // Process each line
                for (ssize_t i = 0; i < bytesRead; ++i)
                {
                    if (buffer[i] == '\n')
                    {
                        string response;
                        // When we encounter a newline, we have a complete log entry
                        synchronizeTracker(logEntry);
                        logEntry.clear(); // Reset for the next log entry
                    }
                    else
                    {
                        logEntry += buffer[i];
                    }
                }
            }
            close(logFileFd); // Close the file after reading
            // Clear the log file by opening with O_TRUNC
            int clearLogFd = open(currentLogFile.c_str(), O_WRONLY | O_TRUNC);
            if (clearLogFd < 0)
            {
                std::cerr << "Error clearing log file: " << strerror(errno) << std::endl;
                return;
            }
            close(clearLogFd); // Close the file after truncating
        }

        // Buffer for reading file content
        memset(buffer, 0, sizeof(buffer));
        int valread = read(client_socket, buffer, sizeof(buffer));
        if (valread <= 0)
        {
            close(client_socket);
            break;
        }
        string currentBuffer = string(buffer) + " " + to_string(client_socket) + " " + client_ip + " " + to_string(client_port) + "\n";
        string response = synchronizeTracker(currentBuffer);
        send(client_socket, response.c_str(), response.size(), 0);
        write_log_file(othertrackerip, othertrackerport, currentBuffer);
    }
    close(client_socket); // Close the client socket when done
}

int main(int argc, char *argv[])
{
    if (argc != 3)
    {
        std::cerr << "Usage: ./tracker tracker_info.txt tracker_no" << std::endl;
        return EXIT_FAILURE;
    }
    const char *tracker_info_file = argv[1];
    int tracker_no = std::stoi(argv[2]);

    // Fetch tracker information from the file
    pair<std::string, int> trackerInfo = getTrackerInfo(tracker_info_file, tracker_no);
    std::string ip = trackerInfo.first;
    int port = trackerInfo.second;

    pair<std::string, int> othertrackerInfo = getTrackerInfo(tracker_info_file, (tracker_no == 1 ? 2 : 1));
    string othertrackerip = othertrackerInfo.first;
    int othertrackerport = othertrackerInfo.second;

    // Create the server socket and start listening
    int server_fd = createServerSocket(ip, port);

    // Output the tracker info for verification
    std::cout << "Starting tracker " << tracker_no << " at " << ip << ":" << port << std::endl;

    int client_socket;
    struct sockaddr_in client_address;
    socklen_t client_addr_len = sizeof(client_address);

    // Create a thread to listen for the "quit" command
    pthread_t quit_thread;
    pthread_create(&quit_thread, NULL, listenForQuit, NULL);

    while (running)
    {
        std::cout << "Waiting for connections " << endl;
        // Accept connections and handle clients
        client_socket = accept(server_fd, (struct sockaddr *)&client_address, &client_addr_len);

        if (client_socket < 0)
        {
            cerr << "Accept failed" << endl;
            continue;
        }
        // Allocate memory for ClientInfo and fill it
        ClientInfo *client_info = new ClientInfo();
        client_info->client_socket = client_socket;
        client_info->client_address = client_address;
        client_info->trackerip = ip;
        client_info->trackerport = to_string(port);
        client_info->othertrackerip = othertrackerip;
        client_info->othertrackerport = to_string(othertrackerport);
        // Handle each client in a separate thread
        pthread_t client_thread;
        pthread_create(&client_thread, NULL, [](void *arg) -> void *
                       {
            // Cast the argument back to ClientInfo pointer
            ClientInfo *client_info = (ClientInfo *)arg;
            handleClient(client_info->client_socket, client_info->client_address, client_info->trackerip, client_info->trackerport, client_info->othertrackerip, client_info->othertrackerport);
            delete client_info; // Free the allocated memory after use
            return NULL; }, client_info);

        char client_ip[INET_ADDRSTRLEN];                                            // Buffer to hold IP address
        inet_ntop(AF_INET, &client_address.sin_addr, client_ip, sizeof(client_ip)); // Convert IP to string
        int client_port = ntohs(client_address.sin_port);                           // Get port number

        std::cout << "Client connected: " << client_ip << ":" << client_port << endl;
    }
    close(server_fd);                // Close the server socket when shutting down
    pthread_join(quit_thread, NULL); // Wait for the quit thread to finish

    return 0;
}

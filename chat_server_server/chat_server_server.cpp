﻿#include <boost/asio.hpp>
#include <boost/bind/bind.hpp>
#include <sqlite3.h>
#include <iostream>
#include <unordered_map>
#include <unordered_set>
#include <string>
#include <memory>
#include <vector>
#include <fstream>

using boost::asio::ip::tcp;
using namespace std;

// 데이터베이스 파일 경로
const string DB_FILE = "data/chat_server.db";

// SQLite 데이터베이스 초기화 함수
void initialize_database() {
    sqlite3* db;
    int rc = sqlite3_open(DB_FILE.c_str(), &db);
    if (rc) {
        cerr << "Can't open database: " << sqlite3_errmsg(db) << endl;
        return;
    }

    // users 테이블 생성
    const char* users_table = "CREATE TABLE users("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "login_id TEXT NOT NULL UNIQUE,"
        "login_password TEXT NOT NULL,"
        "name TEXT NOT NULL UNIQUE"
        ");";
    rc = sqlite3_exec(db, users_table, nullptr, nullptr, nullptr);
    if (rc != SQLITE_OK) {
        cerr << "Failed to create users table: " << sqlite3_errmsg(db) << endl;
    }

    // rooms 테이블 생성
    const char* rooms_table = "CREATE TABLE rooms("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "title TEXT NOT NULL UNIQUE,"
        "host_user_id INTEGER NOT NULL,"
        "FOREIGN KEY(host_user_id) REFERENCES users(id)"
        ");";
    rc = sqlite3_exec(db, rooms_table, nullptr, nullptr, nullptr);
    if (rc != SQLITE_OK) {
        cerr << "Failed to create rooms table: " << sqlite3_errmsg(db) << endl;
    }

    // talks 테이블 생성
    const char* talks_table = "CREATE TABLE talks ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "room_id INTEGER NOT NULL,"
        "user_id INTEGER NOT NULL,"
        "text TEXT NOT NULL,"
        "published_date TEXT NOT NULL,"
        "FOREIGN KEY(room_id) REFERENCES rooms(id),"
        "FOREIGN KEY(user_id) REFERENCES users(id)"
        ");";
    rc = sqlite3_exec(db, talks_table, nullptr, nullptr, nullptr);
    if (rc != SQLITE_OK) {
        cerr << "Failed to create talks table: " << sqlite3_errmsg(db) << endl;
    }

    sqlite3_close(db);
}

// 채팅방 클래스
class ChatRoom {
public:
    void join(shared_ptr<ChatSession> session) {
        members_.insert(session);
        broadcast("A new user has joined the chat.");
    }

    void leave(shared_ptr<ChatSession> session) {
        members_.erase(session);
        // if(members_)
        broadcast("A user has left the chat.");
    }

    void broadcast(const string& message) {
        for (auto& member : members_) {
            boost::asio::write(member->get_socket(), boost::asio::buffer(message + "\n"));
        }
    }

    void save_message_to_db(int room_id, int user_id, const string& message) {
        sqlite3* db;
        int rc = sqlite3_open(DB_FILE.c_str(), &db);
        if (rc) {
            cerr << "Can't open database: " << sqlite3_errmsg(db) << endl;
            return;
        }

        string sql = "INSERT INTO talks (room_id, user_id, text, published_date) VALUES (?, ?, ?, datetime('now'));";
        sqlite3_stmt* stmt;
        rc = sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);
        if (rc != SQLITE_OK) {
            cerr << "Failed to prepare statement: " << sqlite3_errmsg(db) << endl;
            sqlite3_close(db);
            return;
        }

        sqlite3_bind_int(stmt, 1, room_id);
        sqlite3_bind_int(stmt, 2, user_id);
        sqlite3_bind_text(stmt, 3, message.c_str(), -1, SQLITE_STATIC);

        rc = sqlite3_step(stmt);
        if (rc != SQLITE_DONE) {
            cerr << "Failed to insert message: " << sqlite3_errmsg(db) << endl;
        }

        sqlite3_finalize(stmt);
        sqlite3_close(db);
    }

    size_t member_count() const {
        return members_.size();
    }
private:
    unordered_set<shared_ptr<ChatSession>> members_;  // shared_ptr 관리
};

// 채팅 세션 클래스
class ChatSession : public enable_shared_from_this<ChatSession> {
public:
    ChatSession(tcp::socket socket, ChatServer& server)
        : socket_(move(socket)), server_(server) {
    }

    void start(shared_ptr<ChatSession> self) {
        self_ = self;
        read_initial_data();
    }

    void leave_room() {
        server_.get_or_create_room(room_id_).leave(self_);
        server_.remove_empty_room(room_id_); // 사용자가 나간 후 방을 확인하여 제거
    }

    tcp::socket& get_socket() { return socket_; }

private:
    void read_initial_data() {
        auto self = shared_from_this();
        boost::asio::async_read_until(socket_, boost::asio::dynamic_buffer(buffer_), "\n",
            [this, self](boost::system::error_code ec, size_t length) {
                if (!ec) {
                    string data = buffer_.substr(0, length - 1);
                    buffer_.erase(0, length);

                    // room_id와 user_id 파싱
                    parse_initial_data(data);

                    // ChatServer를 통해 적절한 방 찾기
                    ChatRoom& room = server_.get_or_create_room(room_id_);
                    room.join(self);

                    do_read();
                }
                else {
                    cerr << "Error reading initial data: " << ec.message() << endl;
                }
            });
    }

    void parse_initial_data(const string& data) {
        auto comma_pos = data.find(',');
        if (comma_pos != string::npos) {
            room_id_ = stoi(data.substr(0, comma_pos));
            //user_id_ = stoi(data.substr(comma_pos + 1));
        }
        else {
            cerr << "Invalid data format: " << data << endl;
        }
    }

    void do_read() {
        auto self = shared_from_this();
        boost::asio::async_read_until(socket_, boost::asio::dynamic_buffer(buffer_), "\n",
            [this, self](boost::system::error_code ec, size_t length) {
                if (!ec) {
                    string message = buffer_.substr(0, length - 1);
                    buffer_.erase(0, length);
                    server_.get_or_create_room(room_id_).broadcast(message);
                    do_read();
                }
                else {
                    server_.get_or_create_room(room_id_).leave(self);
                }
            });
    }

    tcp::socket socket_;
    ChatServer& server_;
    int room_id_;
    string buffer_;
    shared_ptr<ChatSession> self_;
};


// 채팅 서버 클래스
class ChatServer {
public:
    ChatServer(boost::asio::io_context& io_context, const tcp::endpoint& endpoint)
        : acceptor_(io_context, endpoint) {
        initialize_database(); // 서버 시작 시 데이터베이스 초기화
        do_accept();
    }

    // 특정 room_id에 해당하는 ChatRoom을 반환 (없으면 생성)
    ChatRoom& get_or_create_room(int room_id) {
        auto it = rooms_.find(room_id);
        if (it == rooms_.end()) {
            auto emplace_result = rooms_.emplace(room_id, make_unique<ChatRoom>());
            auto& new_it = emplace_result.first;  // 삽입된 요소의 반복자
            bool success = emplace_result.second; // 삽입 성공 여부
            return *(new_it->second);
        }
        return *(it->second);
    }

    void remove_empty_room(int room_id) {
        auto it = rooms_.find(room_id);
        if (it != rooms_.end() && it->second->member_count() == 0) {
            rooms_.erase(it);
            cout << "Room " << room_id << " has been removed (no members)." << endl;
        }
    }
private:
    void do_accept() {
        acceptor_.async_accept(
            [this](boost::system::error_code ec, tcp::socket socket) {
                if (!ec) {
                    cout << "New connection from " << socket.remote_endpoint() << endl;

                    auto session = make_shared<ChatSession>(move(socket), *this);
                    session->start(session);  // ChatSession에서 room_id와 user_id를 받아 방에 입장
                }
                do_accept();
            });
    }
    void check_message(string message) {
        // str이 무슨 명령을 지시하는지
        // 그 명령에 따라서 어떤 함수를 실행하는지가 정의가 되야한다.

        // 인터페이스쪽에서 보내지는 명령의 종류
        // 메세지 프로토콜 : 함수명?변수명1:데이터1/변수명2:데이터2
        // 1. 회원가입
        //   create_user?id:aaa/password:bbb
        // 2. 로그인
        //   login_user?id:aaa/password:bbb
        // 3. 방생성(유저 초대와 동일 인터페이스가)
        //   create_room?title:gfdsa
        // 4. 방참여
        //   join_room?room_id:1
        // 5. 메세지 보내기
        //   send_text?room_id:1/user_id:1/text:안녕하세요
        // 6. 방 나가기
        //   exit_room?room_id:1/user_id:1
        // 7. 유저 추방
        //   kick_user?room_id:1/user_id:1/target_user_id:2
        // 8. 방장 위임
        //   grant_host?room_id:1/user_id:1/target_user_id:2
        // 9. 유저 초대(방생성과 동일 인터페이스가)
        //   invite_user?room_id:1/user_id:1/target_user_id:2

        // 
    }

    tcp::acceptor acceptor_;
    unordered_map<int, unique_ptr<ChatRoom>> rooms_; // room_id별 ChatRoom 관리
};

class ChatUser {
public:
    int get_userID() { return user_id_; }
    string get_address() { return endpoint_ + ":" + to_string(port_); }
    string get_name() { return name_; }
    void set_login(bool stat) {is_logined_ = stat;}
    bool get_login() { return is_logined_; }
private:
    int user_id_;
    string endpoint_;
    int port_;
    string name_;
    bool is_logined_;
};


int main() {
    try {
        boost::asio::io_context io_context;

        tcp::endpoint endpoint(tcp::v4(), 12345); // 포트 12345에서 수신 대기
        ChatServer server(io_context, endpoint);

        cout << "Chat server is running on port 12345..." << endl;

        io_context.run();
    }
    catch (const exception& e) {
        cerr << "Error: " << e.what() << endl;
    }

    return 0;
}

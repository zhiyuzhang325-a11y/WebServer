#include <cstring>
#include <iostream>
#include <sstream>
using namespace std;

enum ParseState {
    PARSE_REQUEST_LINE,
    PARSE_HEADERS,
    PARSE_BODY,
    PARSE_DONE,
    PARSE_ERROR
};

struct HttpRequest {
    ParseState state = PARSE_REQUEST_LINE;

    string method;
    string target;
    string version;

    string host;
    string content_type;
    int content_length = 0;

    string body;
};

ParseState parse_http_from_string(const string &raw, HttpRequest &req) {
    int index = 0;
    size_t pos;
    string line;
    while (req.state == PARSE_BODY || (pos = raw.find("\r\n", index)) != string::npos) {
        if (req.state == PARSE_REQUEST_LINE || req.state == PARSE_HEADERS) {
            line = raw.substr(index, pos - index);
            index = pos + 2;
        }
        switch (req.state) {
        case PARSE_REQUEST_LINE: {
            istringstream iss(line);
            iss >> req.method >> req.target >> req.version;
            if (req.method.empty() || req.target.empty() || req.version.empty()) {
                req.state = PARSE_ERROR;
                break;
            }
            if (req.method != "GET" && req.method != "POST" && req.method != "HEAD") {
                req.state = PARSE_ERROR;
                break;
            }
            req.state = PARSE_HEADERS;
            break;
        }

        case PARSE_HEADERS: {
            if (line.empty()) {
                if (req.content_length > 0) {
                    req.state = PARSE_BODY;
                } else {
                    req.state = PARSE_DONE;
                }
                break;
            }

            size_t colon_pos = line.find(':');
            if (colon_pos == string::npos) {
                req.state = PARSE_ERROR;
                break;
            }
            string key = line.substr(0, colon_pos);
            string value = line.substr(colon_pos + 2);

            if (key.empty() || value.empty()) {
                req.state = PARSE_ERROR;
            } else if (key == "Host") {
                req.host = value;
            } else if (key == "Content-Type") {
                req.content_type = value;
            } else if (key == "Content-Length") {
                req.content_length = stoi(value);
            }
            break;
        }

        case PARSE_BODY: {
            req.body = raw.substr(index, req.content_length);
            req.state = PARSE_DONE;
            return PARSE_DONE;
        }

        case PARSE_DONE:
            return PARSE_DONE;

        case PARSE_ERROR:
            return PARSE_ERROR;
        }
    }
    return req.state;
}

int main() {
    string req1 =
        "GET /index.html HTTP/1.1\r\n"
        "Host: example.com\r\n"
        "Connection: close\r\n"
        "\r\n";
    string req2 =
        "POST /login HTTP/1.1\r\n"
        "Host: example.com\r\n"
        "Content-Type: application/x-www-form-urlencoded\r\n"
        "Content-Length: 13\r\n"
        "\r\n"
        "name=abc&x=1";
    string req3 =
        "INVALID\r\n"
        "\r\n";

    HttpRequest req;
    ParseState ret = parse_http_from_string(req1, req);

    cout << "===== parse result =====\n";
    cout << "return state: " << ret << '\n';
    cout << "req.state: " << req.state << '\n';
    cout << "method: " << req.method << '\n';
    cout << "target: " << req.target << '\n';
    cout << "version: " << req.version << '\n';
    cout << "host: " << req.host << '\n';
    cout << "content_type: " << req.content_type << '\n';
    cout << "content_length: " << req.content_length << '\n';
    cout << "body: " << req.body << '\n';
    cout << "========================\n";

    req = {};
    ret = parse_http_from_string(req2, req);

    cout << "===== parse result =====\n";
    cout << "return state: " << ret << '\n';
    cout << "req.state: " << req.state << '\n';
    cout << "method: " << req.method << '\n';
    cout << "target: " << req.target << '\n';
    cout << "version: " << req.version << '\n';
    cout << "host: " << req.host << '\n';
    cout << "content_type: " << req.content_type << '\n';
    cout << "content_length: " << req.content_length << '\n';
    cout << "body: " << req.body << '\n';
    cout << "========================\n";

    req = {};
    ret = parse_http_from_string(req3, req);

    cout << "===== parse result =====\n";
    cout << "return state: " << ret << '\n';
    cout << "req.state: " << req.state << '\n';
    cout << "method: " << req.method << '\n';
    cout << "target: " << req.target << '\n';
    cout << "version: " << req.version << '\n';
    cout << "host: " << req.host << '\n';
    cout << "content_type: " << req.content_type << '\n';
    cout << "content_length: " << req.content_length << '\n';
    cout << "body: " << req.body << '\n';
    cout << "========================\n";
}
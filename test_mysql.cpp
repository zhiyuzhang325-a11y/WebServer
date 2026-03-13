#include <iostream>
#include <mysql/mysql.h>

using namespace std;

int main() {
    MYSQL *conn = mysql_init(NULL);

    MYSQL *ret = mysql_real_connect(conn, "127.0.0.1", "root", "123456", "webserver", 3306, NULL, 0);

    if (ret == NULL) {
        cout << "real_connect failed" << endl;
        return -1;
    }

    mysql_query(conn, "SELECT * FROM users");

    MYSQL_RES *result = mysql_store_result(conn);
    unsigned int num_fields = mysql_num_fields(result);
    MYSQL_FIELD *fields = mysql_fetch_fields(result);

    for (int i = 0; i < num_fields; i++) {
        cout << fields[i].name << " ";
    }
    cout << endl;

    MYSQL_ROW row;
    while ((row = mysql_fetch_row(result)) != NULL) {
        for (int i = 0; i < 4; i++) {
            cout << row[i] << " ";
        }
        cout << endl;
    }

    mysql_close(conn);

    return 0;
}
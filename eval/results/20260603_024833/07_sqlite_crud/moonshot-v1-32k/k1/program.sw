module Main

import Std

fun main() {
    db = db_open(":memory:")
    db_exec(db, "CREATE TABLE users(id INTEGER PRIMARY KEY, name TEXT)")
    db_exec(db, "INSERT INTO users VALUES(1, 'alice')")
    db_exec(db, "INSERT INTO users VALUES(2, 'bob')")
    rows = db_query(db, "SELECT * FROM users ORDER BY id", [])
    foreach(rows, fun(row) {
        print(f"id={map_get(row, 'id')} name={map_get(row, 'name')}")
    })
}

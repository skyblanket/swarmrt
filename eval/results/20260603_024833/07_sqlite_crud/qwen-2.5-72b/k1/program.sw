module Main

import Std

fun main() {
    db = db_open(":memory:")
    db_exec(db, "CREATE TABLE users (id INTEGER, name TEXT)")
    db_exec(db, "INSERT INTO users (id, name) VALUES (?, ?)", [1, "alice"])
    db_exec(db, "INSERT INTO users (id, name) VALUES (?, ?)", [2, "bob"])
    
    result = db_query(db, "SELECT * FROM users ORDER BY id", [])
    for row in result {
        id = map_get(row, "id")
        name = map_get(row, "name")
        print(f"id={id} name={name}")
    }
}

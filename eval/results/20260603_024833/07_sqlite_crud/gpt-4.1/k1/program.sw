module Main

fun main() {
    db = db_open(":memory:")
    db_exec(db, "create table users(id int, name text);")
    db_exec(db, "insert into users(id, name) values(1, 'alice');")
    db_exec(db, "insert into users(id, name) values(2, 'bob');")
    rows = db_query(db, "select id, name from users order by id;", [])
    for row in rows {
        id = map_get(row, "id")
        name = map_get(row, "name")
        print(f"id={id} name={name}")
    }
}

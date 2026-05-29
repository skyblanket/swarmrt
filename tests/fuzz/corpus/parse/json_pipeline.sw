# json_pipeline.sw — small data transform with case + f-strings + pipe operator.
#
# Loads a list of users, classifies each by age band using case, prints
# a tidy summary. Demonstrates how the new ergonomics turn what would
# be deeply-nested if/else into something readable.

module JsonPipeline
export [main]

fun classify(age) {
    case age {
        n when n < 13 -> "child"
        n when n < 20 -> "teen"
        n when n < 65 -> "adult"
        _             -> "senior"
    }
}

fun describe(user) {
    name = map_get(user, 'name')
    age = map_get(user, 'age')
    f"{name} ({age}) → {classify(age)}"
}

fun describe_each(users) {
    if (length(users) == 0) { 'done' }
    else {
        print(describe(hd(users)))
        describe_each(tl(users))
    }
}

fun main() {
    json = "[{\"name\":\"alice\",\"age\":7}," ++
           " {\"name\":\"bob\",\"age\":17}," ++
           " {\"name\":\"carol\",\"age\":42}," ++
           " {\"name\":\"dave\",\"age\":71}]"
    users = json_decode(json)
    print(f"loaded {length(users)} users")
    print("---")
    describe_each(users)
}

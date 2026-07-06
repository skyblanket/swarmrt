package main
import "fmt"
func main() {
    ping := make(chan int); pong := make(chan int)
    n := 2000000
    go func() { for { <-ping; pong <- 1 } }()
    for i := 0; i < n; i++ { ping <- 1; <-pong }
    fmt.Println("done")
}

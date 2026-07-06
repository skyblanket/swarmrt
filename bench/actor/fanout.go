package main
import "fmt"
func main() {
    n := 5000000
    ch := make(chan int, 1)
    done := make(chan int)
    go func() { for i := 0; i < n; i++ { <-ch }; done <- 1 }()
    for i := 0; i < n; i++ { ch <- 1 }
    <-done
    fmt.Println("done")
}

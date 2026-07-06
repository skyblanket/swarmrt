package main
import ("fmt";"sync")
func main(){ var wg sync.WaitGroup; n:=1000000
 for i:=0;i<n;i++{ wg.Add(1); go func(){ wg.Done() }() }
 wg.Wait(); fmt.Println("done") }

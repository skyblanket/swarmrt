package main
import ("fmt";"sync")
func main(){ var wg sync.WaitGroup; k:=20; m:=500000
 for p:=0;p<k;p++{ wg.Add(1); ch:=make(chan int,1)
   go func(){ for i:=0;i<m;i++{ <-ch } }()
   go func(){ defer wg.Done(); for i:=0;i<m;i++{ ch<-1 } }() }
 wg.Wait(); fmt.Println("done") }

package main

import "fmt"

const N = 32

func fib(n int) int64 {
	if n < 2 {
		return int64(n)
	}
	return fib(n-1) + fib(n-2)
}

func main() {
	fmt.Println(fib(N))
}

package main

import "fmt"

const N = 195000

func fibMemo(n int, memo map[int]int) int {
	if n < 2 {
		return n
	}
	if v, ok := memo[n]; ok {
		return v
	}
	v := fibMemo(n-1, memo) + fibMemo(n-2, memo)
	memo[n] = v
	return v
}

func main() {
	total := 0
	for r := 0; r < N; r++ {
		memo := make(map[int]int)
		total += fibMemo(25, memo)
	}
	fmt.Println(total)
}

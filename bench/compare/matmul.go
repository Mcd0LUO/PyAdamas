package main

import "fmt"

const N = 120

func makeMatrix(n int) []int64 {
	out := make([]int64, n*n)
	for i := range out {
		out[i] = int64((i*7 + 3) % 11)
	}
	return out
}

func main() {
	a := makeMatrix(N)
	b := makeMatrix(N)
	c := make([]int64, N*N)
	for i := 0; i < N; i++ {
		for k := 0; k < N; k++ {
			aik := a[i*N+k]
			if aik != 0 {
				for j := 0; j < N; j++ {
					c[i*N+j] += aik * b[k*N+j]
				}
			}
		}
	}
	var total int64
	for _, v := range c {
		total += v
	}
	fmt.Println(total)
}

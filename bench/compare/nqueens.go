package main

import "fmt"

const N = 12

func place(row, n int, cols, d1, d2 []int) int {
	if row == n {
		return 1
	}
	total := 0
	for c := 0; c < n; c++ {
		if cols[c] == 0 && d1[row+c] == 0 && d2[row-c+n] == 0 {
			cols[c] = 1
			d1[row+c] = 1
			d2[row-c+n] = 1
			total += place(row+1, n, cols, d1, d2)
			cols[c] = 0
			d1[row+c] = 0
			d2[row-c+n] = 0
		}
	}
	return total
}

func main() {
	cols := make([]int, N)
	d1 := make([]int, 2*N)
	d2 := make([]int, 2*N)
	fmt.Println(place(0, N, cols, d1, d2))
}

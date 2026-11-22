// collatz —— 1..N 的 Collatz 步数总和与最长链（整数分支 / 除法）
package main

import "fmt"

const N = 50000

func steps(n int64) int64 {
	var c int64 = 0
	for n != 1 {
		if n%2 == 0 {
			n /= 2
		} else {
			n = 3*n + 1
		}
		c++
	}
	return c
}

func main() {
	var total, best, bestn int64 = 0, 0, 1
	for i := int64(1); i < N; i++ {
		s := steps(i)
		total += s
		if s > best {
			best = s
			bestn = i
		}
	}
	fmt.Println(total, best, bestn)
}

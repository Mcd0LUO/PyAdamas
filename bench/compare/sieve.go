// sieve —— 埃氏筛计数（list[int] 逐元素写 + 跨步内层循环）
package main

import "fmt"

const N = 500000

func main() {
	flags := make([]int32, N)
	for i := 0; i < N; i++ {
		flags[i] = 1
	}
	flags[0] = 0
	flags[1] = 0
	for p := 2; p*p < N; p++ {
		if flags[p] == 1 {
			for q := p * p; q < N; q += p {
				flags[q] = 0
			}
		}
	}
	count := 0
	for i := 0; i < N; i++ {
		if flags[i] == 1 {
			count++
		}
	}
	fmt.Println(count)
}

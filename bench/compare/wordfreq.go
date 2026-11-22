// wordfreq —— LCG 生成 N 个词并用 dict 计数（dict / str 路径）
package main

import (
	"fmt"
	"strconv"
)

const N = 200000

func main() {
	x := int64(12345)
	counts := make(map[string]int)
	for i := 0; i < N; i++ {
		x = (x*1103515245 + 12345) % 2147483648
		counts["w"+strconv.FormatInt(x%1000, 10)] += 1
	}
	total := 0
	best := 0
	for _, v := range counts {
		total += v
		if v > best {
			best = v
		}
	}
	fmt.Println(len(counts), total, best)
}

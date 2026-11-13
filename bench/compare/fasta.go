package main

import (
	"fmt"
	"strings"
)

const N = 40000

func build(n int) string {
	seed := int64(42)
	var b strings.Builder
	total := 0
	for total < n {
		seed = (seed*1103515245 + 12345) % 2147483648
		switch seed % 4 {
		case 0:
			b.WriteString("ACGT")
		case 1:
			b.WriteString("AAAA")
		case 2:
			b.WriteString("CCCC")
		default:
			b.WriteString("GGGG")
		}
		total += 4
	}
	return b.String()
}

func main() {
	s := build(N)
	h := int64(0)
	for i := 0; i < N; i++ {
		h = (h*31 + int64(s[i])) % 1000000007
	}
	fmt.Println(N)
	fmt.Println(h)
}

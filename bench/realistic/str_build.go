package main

import (
	"fmt"
	"strings"
)

const N = 66000

func buildLine(n int) string {
	var sb strings.Builder
	for i := 0; i < n; i++ {
		sb.WriteByte(byte('0' + i%10))
	}
	return sb.String()
}

func transform(s string) string {
	var sb strings.Builder
	for i := 0; i < len(s); i++ {
		c := s[i]
		if c == '0' {
			sb.WriteByte('_')
		} else {
			sb.WriteByte(c)
		}
	}
	return sb.String()
}

func main() {
	total := 0
	for r := 0; r < N; r++ {
		line := buildLine(60)
		t := transform(line)
		total += len(t)
	}
	fmt.Println(total)
}

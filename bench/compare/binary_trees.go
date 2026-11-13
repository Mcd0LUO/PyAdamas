package main

import "fmt"

const N = 12

type Node struct {
	left  *Node
	right *Node
}

func makeTree(depth int) *Node {
	if depth == 0 {
		return nil
	}
	return &Node{makeTree(depth - 1), makeTree(depth - 1)}
}

func check(node *Node) int {
	if node == nil {
		return 1
	}
	return 1 + check(node.left) + check(node.right)
}

func main() {
	fmt.Println(check(makeTree(N)))
}

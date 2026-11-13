#include <cstdio>

static const int N = 12;

struct Node {
    Node* left;
    Node* right;
};

static Node* make_tree(int depth) {
    if (depth == 0) return nullptr;
    return new Node{make_tree(depth - 1), make_tree(depth - 1)};
}

static long long check(const Node* node) {
    if (!node) return 1;
    return 1 + check(node->left) + check(node->right);
}

int main() {
    std::printf("%lld\n", check(make_tree(N)));
    return 0;
}

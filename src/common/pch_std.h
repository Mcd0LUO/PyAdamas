// common/pch_std.h —— 预编译头候选：全项目 TU 共用的高频标准库头。
// 只放标准库头，不放项目头：项目头改动频繁，进 PCH 会让增量构建频繁失效。
// 由 CMake option PYADAMAS_PCH 控制（target_precompile_headers 按配置匹配 flags，
// dev/release 各自生成自己的 PCH，不会跨 -O 级别复用）。
#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cassert>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cmath>
#include <compare>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <numeric>
#include <optional>
#include <ostream>
#include <queue>
#include <ranges>
#include <set>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

#!/bin/bash
#
# DarwinCore Network 测试运行脚本
#
# 功能：
#   自动构建并运行不同规模的测试
#
# 用法：
#   ./run_tests.sh [all|kqueue|small|medium|large|extreme|clean]
#

set -e

# 颜色定义
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# 打印带颜色的消息
print_header() {
    echo -e "${BLUE}========================================${NC}"
    echo -e "${BLUE}  $1${NC}"
    echo -e "${BLUE}========================================${NC}"
}

print_success() {
    echo -e "${GREEN}✓ $1${NC}"
}

print_error() {
    echo -e "${RED}✗ $1${NC}"
}

print_info() {
    echo -e "${YELLOW}ℹ $1${NC}"
}

# 检查并增加文件描述符限制
check_fd_limit() {
    local current_limit=$(ulimit -n)
    if [ "$current_limit" -lt 4096 ]; then
        print_info "当前文件描述符限制: $current_limit"
        print_info "建议增加到至少 4096"
        print_info "运行: ulimit -n 65536"
        read -p "是否尝试增加限制? (y/n) " -n 1 -r
        echo
        if [[ $REPLY =~ ^[Yy]$ ]]; then
            ulimit -n 65536 2>/dev/null || true
            print_info "新限制: $(ulimit -n)"
        fi
    else
        print_success "文件描述符限制: $current_limit"
    fi
}

# 构建测试
build_tests() {
    print_header "构建测试"

    if [ ! -d "build" ]; then
        mkdir build
    fi

    cd build

    if [ ! -f "Makefile" ]; then
        print_info "运行 CMake..."
        cmake .. || {
            print_error "CMake 配置失败"
            exit 1
        }
    fi

    print_info "编译测试..."
    make -j4 || {
        print_error "编译失败"
        exit 1
    }

    print_success "编译完成"

    cd ..
}

# 运行 kqueue 核心测试
run_kqueue_test() {
    print_header "kqueue 核心机制测试"

    cd build

    if [ -f "./test_kqueue_core" ]; then
        ./test_kqueue_core || {
            print_error "kqueue 核心测试失败"
            cd ..
            return 1
        }
        print_success "kqueue 核心测试通过"
    else
        print_error "test_kqueue_core 未找到，请先运行构建"
        cd ..
        return 1
    fi

    cd ..
}

# 运行基本通信测试
run_basic_test() {
    print_header "基本通信测试"

    cd build

    if [ -f "./server_client_test" ]; then
        ./server_client_test all || {
            print_error "基本通信测试失败"
            cd ..
            return 1
        }
        print_success "基本通信测试通过"
    else
        print_error "server_client_test 未找到，请先运行构建"
        cd ..
        return 1
    fi

    cd ..
}

# 运行压力测试
run_stress_test() {
    local scale=$1

    print_header "压力测试 - $scale"

    cd build

    if [ -f "./test_stress_concurrency" ]; then
        ./test_stress_concurrency "$scale" || {
            print_error "压力测试 ($scale) 失败"
            cd ..
            return 1
        }
        print_success "压力测试 ($scale) 通过"
    else
        print_error "test_stress_concurrency 未找到，请先运行构建"
        cd ..
        return 1
    fi

    cd ..
}

# 清理构建
clean_build() {
    print_header "清理构建"

    if [ -d "build" ]; then
        rm -rf build
        print_success "清理完成"
    else
        print_info "没有需要清理的内容"
    fi
}

# 显示帮助
show_help() {
    cat << EOF
DarwinCore Network 测试运行脚本

用法:
    $0 [选项]

选项:
    all         运行所有测试
    kqueue      仅运行 kqueue 核心机制测试
    basic       仅运行基本通信测试
    small       运行小规模压力测试 (100 连接)
    medium      运行中等规模压力测试 (1000 连接)
    large       运行大规模压力测试 (10000 连接)
    extreme     运行极限规模压力测试 (50000 连接)
    clean       清理构建目录
    help        显示此帮助信息

示例:
    $0 all              # 运行所有测试
    $0 small            # 运行小规模压力测试
    $0 clean            # 清理构建

注意:
    - 运行 large/extreme 测试前请确保系统资源充足
    - 建议增加文件描述符限制: ulimit -n 65536

EOF
}

# 主函数
main() {
    local test_type=${1:-all}

    case $test_type in
        all)
            check_fd_limit
            build_tests
            run_kqueue_test
            run_basic_test
            run_stress_test "small"
            print_success "所有测试完成!"
            ;;
        kqueue)
            build_tests
            run_kqueue_test
            ;;
        basic)
            build_tests
            run_basic_test
            ;;
        small|medium|large|extreme)
            check_fd_limit
            build_tests
            run_stress_test "$test_type"
            ;;
        clean)
            clean_build
            ;;
        help|--help|-h)
            show_help
            ;;
        *)
            print_error "未知选项: $test_type"
            show_help
            exit 1
            ;;
    esac
}

# 运行主函数
main "$@"

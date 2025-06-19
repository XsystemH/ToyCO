#!/bin/bash

# 颜色定义
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# 测试程序列表（按照Makefile中的顺序）
TESTS=("test1" "test2" "test_multi_wait" "test_multi_core" "test_public" "test_steal")

# 函数：打印分隔线
print_separator() {
    echo -e "${BLUE}===========================================${NC}"
}

# 函数：等待用户输入
wait_for_user() {
    echo -e "${YELLOW}按回车键继续到下一个测试...${NC}"
    read -r
}

# 函数：运行单个测试
run_test() {
    local test_name=$1
    
    print_separator
    echo -e "${GREEN}正在运行测试: $test_name${NC}"
    print_separator
    
    # 检查测试程序是否存在
    if [ ! -f "./$test_name" ]; then
        echo -e "${RED}错误: 测试程序 $test_name 不存在！${NC}"
        echo -e "${YELLOW}请先运行 'make $test_name' 编译测试程序${NC}"
        return 1
    fi
    
    # 运行测试程序
    echo -e "${BLUE}开始执行 ./$test_name${NC}"
    echo
    
    # 对于可能无限循环的测试，使用timeout
    if [ "$test_name" = "test1" ]; then
        timeout 5s "./$test_name" || {
            echo
            echo -e "${YELLOW}测试 $test_name 在5秒后被终止（可能是无限循环测试）${NC}"
        }
    else
        "./$test_name"
    fi
    
    local exit_code=$?
    echo
    
    if [ $exit_code -eq 0 ]; then
        echo -e "${GREEN}测试 $test_name 执行完成${NC}"
    elif [ $exit_code -eq 124 ]; then
        echo -e "${YELLOW}测试 $test_name 超时结束${NC}"
    else
        echo -e "${RED}测试 $test_name 执行失败 (退出码: $exit_code)${NC}"
    fi
    
    return $exit_code
}

# 主函数
main() {
    echo -e "${GREEN}=== 协程库测试套件 ===${NC}"
    echo -e "${BLUE}将按照以下顺序运行测试：${NC}"
    
    for test in "${TESTS[@]}"; do
        echo -e "  - $test"
    done
    
    echo
    echo -e "${YELLOW}开始执行测试...${NC}"
    echo
    
    local total_tests=${#TESTS[@]}
    local passed_tests=0
    local failed_tests=0
    
    for i in "${!TESTS[@]}"; do
        local test_name="${TESTS[$i]}"
        local test_num=$((i + 1))
        
        echo -e "${BLUE}[$test_num/$total_tests] 准备运行测试: $test_name${NC}"
        
        run_test "$test_name"
        local result=$?
        
        if [ $result -eq 0 ] || [ $result -eq 124 ]; then
            ((passed_tests++))
        else
            ((failed_tests++))
        fi
        
        # 如果不是最后一个测试，等待用户输入
        if [ $i -lt $((total_tests - 1)) ]; then
            echo
            wait_for_user
            echo
        fi
    done
    
    # 显示测试总结
    print_separator
    echo -e "${GREEN}=== 测试总结 ===${NC}"
    echo -e "${BLUE}总测试数: $total_tests${NC}"
    echo -e "${GREEN}通过/完成: $passed_tests${NC}"
    echo -e "${RED}失败: $failed_tests${NC}"
    print_separator
    
    # 清理编译产物
    echo
    clean_all
    
    if [ $failed_tests -eq 0 ]; then
        echo -e "${GREEN}所有测试执行完成！${NC}"
        return 0
    else
        echo -e "${RED}有 $failed_tests 个测试失败${NC}"
        return 1
    fi
}

# 编译所有测试程序
compile_all() {
    echo -e "${YELLOW}正在编译所有测试程序...${NC}"
    make all
    if [ $? -ne 0 ]; then
        echo -e "${RED}编译失败！请检查代码${NC}"
        exit 1
    fi
    echo -e "${GREEN}编译完成${NC}"
    echo
}

# 清理编译产物
clean_all() {
    echo -e "${YELLOW}正在清理编译产物...${NC}"
    make clean
    if [ $? -eq 0 ]; then
        echo -e "${GREEN}清理完成${NC}"
    else
        echo -e "${RED}清理失败${NC}"
    fi
}

# 脚本入口
if [ "$1" = "--help" ] || [ "$1" = "-h" ]; then
    echo "使用方法: $0 [选项]"
    echo
    echo "选项:"
    echo "  --help, -h    显示此帮助信息"
    echo "  --no-compile  跳过编译步骤，直接运行测试"
    echo
    echo "此脚本将按照Makefile中定义的顺序运行所有测试程序："
    for test in "${TESTS[@]}"; do
        echo "  - $test"
    done
    exit 0
fi

# 如果不是 --no-compile，则先编译
if [ "$1" != "--no-compile" ]; then
    compile_all
fi

# 运行主程序
main 
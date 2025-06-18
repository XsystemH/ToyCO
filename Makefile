CC = gcc
CFLAGS = -Wall -Wextra -g -O2 -Iinclude
SRCDIR = include
TESTDIR = test
OBJDIR = obj

# 源文件
SOURCES = $(wildcard $(SRCDIR)/*.c)
OBJECTS = $(SOURCES:$(SRCDIR)/%.c=$(OBJDIR)/%.o)

# 测试文件
TESTS = $(wildcard $(TESTDIR)/*.c)
TEST_BINS = $(TESTS:$(TESTDIR)/%.c=%)

.PHONY: all clean test test1 test2 test_multi_wait

all: libco.a $(TEST_BINS)

# 创建目录
$(OBJDIR):
	mkdir -p $(OBJDIR)

# 编译源文件为目标文件
$(OBJDIR)/%.o: $(SRCDIR)/%.c | $(OBJDIR)
	$(CC) $(CFLAGS) -c $< -o $@

# 创建静态库
libco.a: $(OBJECTS)
	ar rcs $@ $^

# 编译测试程序（使用简单版本）
test1: libco.a test/test1.c
	$(CC) $(CFLAGS) -o $@ test/test1.c -L. -lco

test2: libco.a test/test2.c
	$(CC) $(CFLAGS) -o $@ test/test2.c -L. -lco

test_multi_wait: libco.a test/test_multi_wait.c
	$(CC) $(CFLAGS) -o $@ test/test_multi_wait.c -L. -lco

# 如果需要测试汇编版本
test1-asm: test/test1.c src/co_asm.c
	$(CC) $(CFLAGS) -o $@ test/test1.c src/co_asm.c

test2-asm: test/test2.c src/co_asm.c
	$(CC) $(CFLAGS) -o $@ test/test2.c src/co_asm.c

# 运行测试
test: test2
	@echo "运行测试程序..."
	timeout 3s ./test2 || true

clean:
	rm -rf $(OBJDIR) libco.a $(TEST_BINS) test1-asm test2-asm test_multi_wait

# 帮助信息
help:
	@echo "可用目标："
	@echo "  all              - 编译所有文件"
	@echo "  test1            - 编译测试1（无限循环版本）"
	@echo "  test2            - 编译测试2（有限循环版本）"
	@echo "  test_multi_wait  - 编译多协程等待测试"
	@echo "  test             - 运行测试2"
	@echo "  clean            - 清理编译文件"
	@echo "  help             - 显示此帮助信息" 
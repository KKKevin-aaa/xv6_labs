#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

static void
fail(const char *msg)
{
  printf("btdebugtest: FAIL: %s\n", msg);
  exit(1);
}

int
main(int argc, char *argv[])
{
  (void)argc;
  (void)argv;

  printf("btdebugtest: begin\n");

  // init 通常已经调用过一次 load_debug_sym()。
  // 这里再调用一次，用来覆盖“防重复加载/重入检测”的新增逻辑。
  int r = load_debug_sym();
  printf("btdebugtest: load_debug_sym() ret=%d\n", r);
  if(r != -1)
    fail("expected load_debug_sym() to return -1 (already loaded)");

  // 触发内核 backtrace（sys_pause() 末尾会调用 backtrace()）。
  // 你新增的输出通常包含：
  //  - backtrace header + kernel stack range
  //  - 每一帧的定位信息（例如 kernel/sysproc.c:... (0x...)）
  printf("btdebugtest: trigger pause/backtrace #1\n");
  if(pause(1) != 0)
    fail("pause(1) returned non-zero");

  // 再 fork 一次触发第二次，覆盖多进程场景下的输出稳定性。
  int pid = fork();
  if(pid < 0)
    fail("fork failed");
  if(pid == 0){
    printf("btdebugtest(child): trigger pause/backtrace #2\n");
    if(pause(1) != 0)
      fail("child pause(1) returned non-zero");
    printf("btdebugtest(child): done\n");
    exit(0);
  }

  wait(0);
  printf("btdebugtest: passed (check kernel output between markers)\n");
  exit(0);
}

// 递归设备函数（C11 余项）的现状锁定。
//
// 结论（本机实测，sm_89 / 驱动 595.58）：
//   1. **递归设备函数是可用的**：PTX 里是真 `.func` + `call.uni`（hacc 不做内联），
//      sm_89 的硬件调用栈能执行，浅递归结果正确。
//   2. **深度受 per-thread 调用栈限制**：默认栈只有 1 KiB → 约 30 层以内正常
//      （实测 n=30 OK、n=50 起失败）。调大需 `cuCtxSetLimit(CU_LIMIT_STACK_SIZE=0x00, …)`，
//      coolrt 目前未封装；实测调到 64 KiB 后 n=200 可通过。
//   3. **溢出是可观测的**：`coolDeviceSynchronize` 返回 999（cudaErrorUnknown），
//      结果槽位保持初值。**不崩溃**，但也不算「成功」。
//
// 因此这里断言的不是「递归能不能用」（能用），而是两条更重要的性质：
//   - 浅递归必须算对（防止有人以「递归不支持」为由加拦截，砍掉有效用法）；
//   - 深递归**要么算对、要么报错**，绝不允许「er=0 但结果是错的」。
#include <cool/cool_runtime.h>

#include <stdio.h>

__device__ long long rec_sum(int n) {
    if (n <= 1) {
        return 1;
    }
    return n + rec_sum(n - 1);  // 期望 n*(n+1)/2
}

__global__ void k(long long* out, int n) { out[0] = rec_sum(n); }

// 跑一轮；返回 0 = 符合预期。
static int run_case(int n, int expect_ok, const char* tag) {
    long long* d = nullptr;
    if (coolMalloc((void**)&d, sizeof(long long)) != coolSuccess) {
        printf("recursion SKIP: 无法分配设备内存（无可用的 CUDA 设备）\n");
        return 77;
    }
    const long long sentinel = -1;
    coolMemcpy(d, &sentinel, sizeof(long long), coolMemcpyHostToDevice);
    k<<<1, 1>>>(d, n);
    const int sync_err = (int)coolDeviceSynchronize();
    long long got = -1;
    coolMemcpy(&got, d, sizeof(long long), coolMemcpyDeviceToHost);
    coolFree(d);

    const long long want = (long long)n * (n + 1) / 2;
    const int got_ok = (sync_err == 0 && got == want);
    if (expect_ok) {
        if (!got_ok) {
            printf("recursion FAILED[%s]: n=%d sync_er=%d got=%lld want=%lld\n", tag, n,
                   sync_err, got, want);
            return 1;
        }
        printf("recursion %s: n=%d -> %lld（正确）\n", tag, n, got);
        return 0;
    }
    // 深递归：算对（栈够大）或报错（栈溢出）都算 OK；只有「静默错值」才是缺陷。
    if (sync_err == 0 && got != want) {
        printf("recursion FAILED[%s]: 静默错值 n=%d got=%lld want=%lld\n", tag, n, got, want);
        return 1;
    }
    if (sync_err != 0) {
        printf("recursion %s: n=%d 栈溢出（sync_er=%d）—— 可观测，符合预期\n", tag, n, sync_err);
    } else {
        printf("recursion %s: n=%d -> %lld（本机栈足够大，也符合预期）\n", tag, n, got);
    }
    return 0;
}

int main() {
    // 1) 浅递归：必须算对。
    const int shallow = run_case(10, 1, "shallow");
    if (shallow != 0) {
        return shallow;
    }
    // 2) 深递归：要么对、要么报错，不许静默错值。
    if (run_case(100000, 0, "deep") != 0) {
        return 1;
    }
    printf("recursion OK\n");
    return 0;
}

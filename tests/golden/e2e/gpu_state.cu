// C4 跳过机制的状态探针：把「这台机器没有 GPU」与「有 GPU 但初始化失败」分开。
//
// 为什么需要它：所有 e2e 样例在拿不到设备时返回 77，ctest 记作 SKIP —— 于是
// `nvidia_uvm` 未加载这类**真故障**和「这台机器没 GPU」表现得一模一样，都是"没失败"。
// 本用例把两种情况拆开，让真故障变红：
//
//   - 枚举到设备                  → 0（OK）
//   - 有 /dev/nvidia0 但枚举不到  → 1（**FAIL**，初始化失败）
//   - 连 /dev/nvidia0 都没有      → 77（SKIP，无 GPU 主机的正常路径）
//
// 判定依据是「设备节点在不在」而不是错误码本身：cuInit 失败时驱动返回 999
// （coolErrorUnknown），而「根本没有卡」时上层也拿不到设备，两者错误码未必稳定可区分，
// 设备节点是更硬的事实（/dev/nvidia0 由驱动/toolkit 注入，与 uvm 加载与否无关）。
#include <cool/cool_runtime.h>

#include <stdio.h>

// hacc 需要至少一个内核才走完整链路；本用例不为它启动（设备不可用时也无法启动）。
__global__ void gpu_state_noop(int* out) { out[0] = 0; }

// 只用 `extern "C"` 声明而**不** include <stdlib.h>：libstdc++ 也提供 stdlib.h wrapper，
// 会像 <math.h> 那样把 c++config.h 拉进 EDG（见 wmma_e2e.cu 的踩坑记录）。
extern "C" char* getenv(const char* name);

static int node_present(const char* path) {
    FILE* f = fopen(path, "r");
    if (f == nullptr) {
        return 0;
    }
    fclose(f);
    return 1;
}

int main() {
    int count = -1;
    coolError_t err = coolGetDeviceCount(&count);
    int has_node = node_present("/dev/nvidia0");

    // 自测模式：本机有 GPU 时也必须能验证「无设备」与「初始化失败」两条分支，
    // 否则这两条分支等于没测过（真机故障不可按需制造）。
    //   HACC_GPU_STATE_FAULT=n → 模拟无设备节点
    //   HACC_GPU_STATE_FAULT=i → 模拟有节点但枚举失败
    char* fault = getenv("HACC_GPU_STATE_FAULT");
    if (fault != nullptr && fault[0] != 0) {
        count = 0;
        err = coolErrorUnknown;
        has_node = (fault[0] == 'i') ? 1 : 0;
        printf("gpuState NOTE: 自测模式 HACC_GPU_STATE_FAULT=%c（不代表本机真实状态）\n", fault[0]);
    }

    if (err == coolSuccess && count > 0) {
        printf("gpuState OK: %d device(s)\n", count);
        return 0;
    }

    if (has_node) {
        printf("gpuState FAILED: /dev/nvidia0 存在但枚举不到设备（err=%d, count=%d）\n", (int)err,
               count);
        printf("gpuState HINT: nvidia-smi 正常 ≠ CUDA 可用，先查 nvidia_uvm ——\n");
        printf("gpuState HINT: lsmod | grep nvidia_uvm; ls -l /dev/nvidia-uvm; grep -i uvm "
               "/proc/devices\n");
        return 1;
    }

    printf("gpuState WARNING: 本机没有 /dev/nvidia0 —— 所有 GPU 用例将 SKIP（不是通过）\n");
    return 77;
}

; M1.1 golden 输入：最小内核的 LLVM IR（手写，模拟 M1.2 的 IL→IR 产物）。
; 期望 coolicc --emit=ptx 产出含 `.visible .entry empty_kernel` 的 PTX。
target triple = "nvptx64-nvidia-cuda"

define ptx_kernel void @empty_kernel() {
  ret void
}

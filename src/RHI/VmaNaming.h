#pragma once

// ===========================================================================
// 给 VMA 分配起名字的辅助。
//
// 必须在 vk_mem_alloc.h **之后**包含（要用到 VMA_ALLOCATION_CREATE_... 常量）。
// Device.h 已经在两者之间排好了顺序，所以其它文件只要用 Device.h 就行。
//
// 为什么值得加这一层：
//   没有名字时，泄漏报告只有
//     "UNFREED ALLOCATION; Size: 1216; Type: BUFFER; Usage: 16; Name: vma_empty"
//   光看这行是猜不出谁漏的，只能靠"改代码 -> 插检查点 -> 重建"二分定位，
//   一轮十几分钟，而且很容易在过程中把真正的 bug 改没。
//   有了名字，一眼就能看出漏的是哪个缓冲。
//
// 用法（三行）：
//   allocInfo.flags |= sentinel_vmaNameFlag();
//   allocInfo.pUserData = const_cast<char*>("名字");
//   vmaCreateBuffer(..., &allocInfo, ...);
//
// 非泄漏检查构建下 sentinel_vmaNameFlag() 恒返回 0，调用点可以无条件写。
// ===========================================================================

inline VkFlags sentinel_vmaNameFlag()
{
#ifdef SENTINEL_VMA_LEAK_CHECK
    // 这一位会让 VMA 把 pUserData 指向的字符串拷进分配里，
    // 于是泄漏报告里的 Name: 就不再是 vma_empty
    return VMA_ALLOCATION_CREATE_USER_DATA_COPY_STRING_BIT;
#else
    return 0;
#endif
}

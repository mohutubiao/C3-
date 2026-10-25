// ── 板型无关的电机接口兜底实现 ──────────────────────────────────────
//   web_server.cc 对下面两个函数是【强引用】（声明里没有 weak 属性），
//   而它们的定义只存在于 qebabe-xiaoche 系列板型。换成 xmini-c3 / WHA103
//   这类没有电机的板型时，链接会直接失败：
//       undefined reference to `HandleMotorActionForDance'
//       undefined reference to `HandleMotorActionForEmotion'
//
//   这里给【弱定义】：
//     - xiaoche 板型里那份强定义会被链接器优先选中，行为不变；
//     - 其他板型用这里的空实现（本来也没有电机可驱动）。
// ────────────────────────────────────────────────────────────────────
#include <cstdint>

extern "C" {

__attribute__((weak)) void HandleMotorActionForEmotion(const char* emotion) {
    (void)emotion;
}

__attribute__((weak)) void HandleMotorActionForDance(uint8_t speed_percent) {
    (void)speed_percent;
}

}  // extern "C"

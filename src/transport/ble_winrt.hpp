// ---------------------------------------------------------------------------
// DGLab_IDAC — BLE 传输工厂声明
// ---------------------------------------------------------------------------
#pragma once

#include "config.hpp"
#include "transport/transport.hpp"

namespace dglab_idac {

// 本机不支持 BLE(WinRT 激活失败 / 没有蓝牙适配器)时返回 nullptr。
// 调用方(create_transport)负责降级到 NullTransport。
ITransport* create_ble_transport(const Config& cfg);

}  // namespace dglab_idac

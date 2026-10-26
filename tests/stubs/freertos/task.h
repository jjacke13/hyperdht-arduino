#pragma once
using TaskHandle_t = void *;
inline TaskHandle_t xTaskGetCurrentTaskHandle() { return reinterpret_cast<void *>(1); }

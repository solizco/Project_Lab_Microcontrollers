#pragma once
extern int g_reset_reason;
inline int esp_reset_reason() { return g_reset_reason; }

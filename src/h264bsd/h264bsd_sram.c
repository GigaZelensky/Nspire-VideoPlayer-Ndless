#include "h264bsd_sram.h"

bool h264bsdInitClipTable(void);
bool h264bsdInitQpCTable(void);
bool h264bsdInitDeblockingTables(void);

static bool g_h264_clip_in_sram = false;
static bool g_h264_qpc_in_sram = false;
static bool g_h264_deblocking_in_sram = false;

bool h264bsdInitSramTables(void)
{
    g_h264_clip_in_sram = h264bsdInitClipTable();
    g_h264_qpc_in_sram = h264bsdInitQpCTable();
    g_h264_deblocking_in_sram = h264bsdInitDeblockingTables();
    return g_h264_clip_in_sram &&
        g_h264_qpc_in_sram &&
        g_h264_deblocking_in_sram;
}

void h264bsdGetSramStatus(bool *clip_in_sram, bool *qpc_in_sram, bool *deblocking_in_sram)
{
    if (clip_in_sram) {
        *clip_in_sram = g_h264_clip_in_sram;
    }
    if (qpc_in_sram) {
        *qpc_in_sram = g_h264_qpc_in_sram;
    }
    if (deblocking_in_sram) {
        *deblocking_in_sram = g_h264_deblocking_in_sram;
    }
}

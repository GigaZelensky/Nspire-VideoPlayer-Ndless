#include "player_internal.h"

void queue_os_suspend_shortcut(void)
{
    struct s_ns_event event;
    /* TI's translated virtual Ctrl+On code (already includes the modifier):
     * https://github.com/debrouxl/tilibs/blob/master/libticalcs/trunk/src/keysnsp.h
     * KEYNSP_CTRL_ON = 0x00F000. This is not a keypad matrix coordinate.
     * Queue only after video/history/LCD/timer cleanup, then return to the OS
     * event loop so the OS owns its normal save-and-suspend sequence. */
    const unsigned short ctrl_on = 0xf000U;
    if (!nl_hassyscall(send_key_event))
        return;
    memset(&event, 0, sizeof(event));
    event.key = 0xf0U;
    event.type = 0x8;
    send_key_event(&event, ctrl_on, FALSE, FALSE);
    /* Reset in case the OS event helper changed the caller's scratch event. */
    memset(&event, 0, sizeof(event));
    event.key = 0xf0U;
    event.type = 0x10;
    send_key_event(&event, ctrl_on, TRUE, FALSE);
}

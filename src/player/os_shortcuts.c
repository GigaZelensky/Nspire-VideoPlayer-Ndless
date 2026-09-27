#include "player_internal.h"
#include "native_interrupts.h"
#include "../platform/portable_reader_platform.h"

static uint32_t redraw_event_post(const PortableView *view, uint32_t key_sender)
{
    /* The SDK key sender reaches the common event dispatcher. Recognize its
     * entire mask/filter wrapper, including the tail branch into the queue.
     * Both CX families use this ABI; no firmware addresses or version table. */
    static const uint32_t post_code[] = {
        0xE59F1018U, 0xE5D03004U, 0xE1D120F0U, 0xE0123003U,
        0x1A000001U, 0xE1A00003U, 0xE12FFF1EU, 0xEAFFFFDBU
    };
    uint32_t found = 0;
    if (!view || !view->allow_span(view->context, key_sender, 0x400U, PORTABLE_CODE))
        return 0;
    for (unsigned offset = 0; offset < 0x400U; offset += 4U) {
        uint32_t instruction;
        if (!view->read_word(view->context, key_sender + offset, &instruction))
            return 0;
        if ((instruction & 0xFF000000U) != 0xEB000000U)
            continue;
        int64_t target = (int64_t)key_sender + offset + 8 + ((int32_t)(instruction << 8) >> 6);
        if (target < view->code_begin + 0x70U || target > view->code_end - 36U ||
            !view->allow_span(view->context, (uint32_t)target - 0x70U, 0x94U, PORTABLE_CODE))
            continue;
        unsigned i;
        for (i = 0; i < sizeof(post_code) / sizeof(post_code[0]); ++i) {
            uint32_t actual;
            if (!view->read_word(view->context, (uint32_t)target + i * 4U, &actual) ||
                actual != post_code[i])
                break;
        }
        if (i != sizeof(post_code) / sizeof(post_code[0]))
            continue;
        if (found && found != (uint32_t)target)
            return 0;
        found = (uint32_t)target;
    }
    return found;
}

static bool redraw_code(const PortableView *view, uint32_t address, const uint32_t *code, unsigned count)
{
    if (!view->allow_span(view->context, address, count * 4U, PORTABLE_CODE))
        return false;
    for (unsigned i = 0; i < count; ++i) {
        uint32_t actual, mask = (code[i] & 0xFF000000U) == 0xEB000000U ? 0xFF000000U : UINT32_MAX;
        if (!view->read_word(view->context, address + i * 4U, &actual) ||
            (actual & mask) != (code[i] & mask))
            return false;
    }
    return true;
}

static uint32_t redraw_call(const PortableView *view, uint32_t pc)
{
    uint32_t instruction;
    if (!view->read_word(view->context, pc, &instruction) ||
        (instruction & 0xFF000000U) != 0xEB000000U)
        return 0;
    int64_t target = (int64_t)pc + 8 + ((int32_t)(instruction << 8) >> 6);
    return target >= view->code_begin && target <= view->code_end - 4U &&
                   view->allow_span(view->context, (uint32_t)target, 4U, PORTABLE_CODE)
               ? (uint32_t)target : 0;
}

static void redraw_enqueue(const PortableView *view, uint32_t post)
{
    /* The ordinary event wrapper may filter repaint messages while an app is
     * open, and its queue send can wait forever when full. Resolve the same
     * queue/serializer, then submit one repaint with a zero wait timeout. */
    static const uint32_t queue_code[] = {
        0xE92D4070U,0xE59F4058U,0xE59F3058U,0xE5941000U,0xE24DD020U,
        0xE591200CU,0xE1A05000U,0xE1520003U,0x13E06000U,0x03A06000U,
        0xEB000000U,0xE28D4004U,0xE1A01005U,0xE5850000U,0xE1A00004U,
        0xEB000000U,0xE1A01004U,0xE1A03006U,0xE3A02007U,0xE59F0018U,
        0xEB000000U,0xE2700001U,0x33A00000U,0xE28DD020U,0xE8BD8070U
    };
    static const uint32_t pack_code[] = {
        0xE5912000U,0xE5802000U,0xE5D13004U,0xE5D1C005U,0xE1D120B6U,0xE1A03C03U,
        0xE183380CU,0xE183C002U,0xE580C004U,0xE1D130BAU,0xE1D120F8U,0xE183C802U,
        0xE580C008U,0xE591300CU,0xE580300CU,0xE591C010U,0xE580C010U,0xE1D131B6U,
        0xE1D121F4U,0xE183C802U,0xE580C014U,0xE1D131B8U,0xE5803018U,0xE12FFF1EU
    };
    if (!post || !redraw_code(view, post - 0x70U, queue_code, sizeof(queue_code) / 4U))
        return;
    uint32_t queue, magic;
    uint32_t pack = redraw_call(view, post - 0x34U);
    uint32_t send = redraw_call(view, post - 0x20U);
    uint32_t clock = redraw_call(view, post - 0x48U);
    if (!pack || !send || !clock || !redraw_code(view, pack, pack_code, sizeof(pack_code) / 4U) ||
        !view->read_word(view->context, post - 4U, &queue) ||
        !view->allow_span(view->context, queue, 0x60U, PORTABLE_DATA) ||
        !view->read_word(view->context, queue + 12U, &magic) || magic != 0x51554555U)
        return;
    /* Serialized TI_HAL_InvalRect event for the complete LCD. Only the GUI's
     * current view is repainted; no key or browser command is generated. */
    uint32_t event[7] = {0, 0x20000000U | SCREEN_W, SCREEN_H << 16, 0, 0, 0x1B710000U, 0};
    event[0] = ((uint32_t (*)(void))(uintptr_t)clock)();
    ((int (*)(void *, const void *, unsigned, unsigned))(uintptr_t)send)(
        (void *)(uintptr_t)queue, event, 7U, 0U);
}

void queue_os_redraw(void)
{
    if (nl_loaded_by_3rd_party_loader() || !nl_hassyscall(send_key_event))
        return;
    unsigned saved = native_interrupt_mask();
    register uint32_t address __asm__("r0");
    __asm__ volatile("swi %[callnr]"
                     : "=r"(address)
                     : [callnr] "i"(__SYSCALLS_ISVAR | e_send_key_event)
                     : "r1", "r2", "r3", "r4", "r12", "lr", "memory", "cc");
    uint32_t key_sender = address;
    native_critical_enter();
    PortableReaderPlatform mapping = {0};
    if (portable_reader_platform_refresh(&mapping)) {
        uint32_t post = redraw_event_post(portable_reader_platform_view(&mapping), key_sender);
        redraw_enqueue(portable_reader_platform_view(&mapping), post);
    }
    native_critical_leave(saved);
}

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

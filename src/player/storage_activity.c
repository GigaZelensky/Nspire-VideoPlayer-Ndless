#include "storage_activity.h"
#include "app_task_io.h"
static unsigned native_transactions;
static uint32_t native_revision = 1U;
void storage_native_begin(void)
{
    unsigned mask = app_task_io_critical_enter();
    ++native_transactions;
    if (native_revision)
        ++native_revision; /* Zero disables reuse after wrap. */
    app_task_io_critical_leave(mask);
}
void storage_native_end(void)
{
    unsigned mask = app_task_io_critical_enter();
    if (native_transactions)
        --native_transactions;
    app_task_io_critical_leave(mask);
}
bool storage_native_active(void)
{
    unsigned mask = app_task_io_critical_enter();
    bool active = native_transactions != 0;
    app_task_io_critical_leave(mask);
    return active;
}
uint32_t storage_native_revision(void) { return native_revision; }

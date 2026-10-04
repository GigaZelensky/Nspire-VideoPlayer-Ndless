/* SPDX-License-Identifier: BSD-2-Clause */

#ifndef DAV1D_SERIAL_ATOMICS_H
#define DAV1D_SERIAL_ATOMICS_H
/* Only for the explicitly single-thread decoder port. */
typedef int atomic_int;
typedef unsigned atomic_uint;
typedef enum {memory_order_relaxed,memory_order_consume,memory_order_acquire,memory_order_release,memory_order_acq_rel,memory_order_seq_cst} memory_order;
#define atomic_init(p,v) (*(p)=(v))
#define atomic_load(p) (*(p))
#define atomic_load_explicit(p,o) (*(p))
#define atomic_store(p,v) (*(p)=(v))
#define atomic_fetch_add(p,v) __extension__ ({__typeof__(p) ap=(p);__typeof__(*ap) old=*ap;*ap=old+(v);old;})
#define atomic_fetch_add_explicit(p,v,o) atomic_fetch_add(p,v)
#define atomic_fetch_sub(p,v) atomic_fetch_add(p,-(v))
#define atomic_fetch_or(p,v) __extension__ ({__typeof__(p) ap=(p);__typeof__(*ap) old=*ap;*ap=old|(v);old;})
#define atomic_exchange(p,v) __extension__ ({__typeof__(p) ap=(p);__typeof__(*ap) old=*ap;*ap=(v);old;})
#define atomic_compare_exchange_strong(p,e,v) __extension__ ({__typeof__(p) ap=(p);__typeof__(e) ae=(e);int okay=*ap==*ae;if(okay)*ap=(v);else *ae=*ap;okay;})
#endif

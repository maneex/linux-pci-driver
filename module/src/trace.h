// https://www.kernel.org/doc/Documentation/trace/tracepoints.rst

#undef TRACE_SYSTEM
#define TRACE_SYSTEM velocitor

// The include guard is deliberately conditional: define_trace.h re-reads this
// header to emit the generated code, and TRACE_HEADER_MULTI_READ is how it
// asks to be let through a second time.
#if !defined(VELOCITOR_TRACE_H) || defined(TRACE_HEADER_MULTI_READ)
#define VELOCITOR_TRACE_H

// Linux headers.
#include <linux/tracepoint.h>
TRACE_EVENT(velocitor_irq, TP_PROTO(unsigned int vector), TP_ARGS(vector),
            TP_STRUCT__entry(__field(unsigned int, vector)),
            TP_fast_assign(__entry->vector = vector;),
            TP_printk("vector=%u", __entry->vector));

TRACE_EVENT(velocitor_winmove, TP_PROTO(u32 from, u32 to, void *caller),
            TP_ARGS(from, to, caller),
            TP_STRUCT__entry(__field(u32, from) __field(u32, to)
                                 __field(void *, caller)),
            TP_fast_assign(__entry->from = from; __entry->to = to;
                           __entry->caller = caller),
            TP_printk("from=%x to=%x caller=%pS", __entry->from, __entry->to,
                      __entry->caller));

TRACE_EVENT(
    velocitor_dma_dbg,
    TP_PROTO(u32 dir, u32 off, u32 poff, u32 len, int res, u32 status, u32 err),
    TP_ARGS(dir, off, poff, len, res, status, err),
    TP_STRUCT__entry(__field(u32, dir) __field(u32, off) __field(u32, poff)
                         __field(u32, len) __field(int, res)
                             __field(u32, status) __field(u32, err)),
    TP_fast_assign(__entry->dir = dir; __entry->off = off; __entry->poff = poff;
                   __entry->len = len; __entry->res = res;
                   __entry->status = status; __entry->err = err;),
    TP_printk("dir=%s off=%#x poff=%#x len=%u res=%d status=%u err=%u",
              __print_symbolic(__entry->dir, {VEL_DBG_DMA_H2D, "H2D"},
                               {VEL_DBG_DMA_D2H, "D2H"}),
              __entry->off, __entry->poff, __entry->len, __entry->res,
              __entry->status, __entry->err));

#endif // not VELOCITOR_TRACE_H

#undef TRACE_INCLUDE_PATH
#define TRACE_INCLUDE_PATH .
#undef TRACE_INCLUDE_FILE
#define TRACE_INCLUDE_FILE trace
#include <trace/define_trace.h>

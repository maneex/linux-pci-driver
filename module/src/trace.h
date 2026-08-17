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

#endif // not VELOCITOR_TRACE_H

#undef TRACE_INCLUDE_PATH
#define TRACE_INCLUDE_PATH .
#undef TRACE_INCLUDE_FILE
#define TRACE_INCLUDE_FILE trace
#include <trace/define_trace.h>

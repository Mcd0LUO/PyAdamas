#ifndef PYADAMAS_CFI_PROBE_H
#define PYADAMAS_CFI_PROBE_H

/* CFI 探针：数一数从当前帧能往上展开多少层（libgcc _Unwind_Backtrace）。 */
long probe_depth(void);

#endif

#!/usr/bin/env python3
"""File-backed SDR input simulation: fixed sample clock, bounded FIFO, no pacing
slowdown under downstream backpressure. Overflow drops incoming samples and is
reported explicitly. Does not open radio hardware. All timings are observable;
this is an OS-scheduled source, not a hardware real-time clock.
"""
import argparse
from collections import deque
import json
import os
from pathlib import Path
import select
import sys
import time


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--in',dest='path',type=Path,required=True)
    p.add_argument('--sample-rate',type=int,required=True)
    p.add_argument('--input-format',choices=('s16','u8'),required=True)
    p.add_argument('--buffer-ms',type=int,default=100)
    a=p.parse_args();rate=a.sample_rate;stride=4 if a.input_format=='s16' else 2
    if rate<100 or a.buffer_ms<10:p.error('rate >= 100 and buffer-ms >= 10 required')
    size=a.path.stat().st_size
    if size%stride:p.error('incomplete IQ sample')
    total=size//stride;chunk=max(1,rate//100);capacity=rate*a.buffer_ms//1000*stride
    queue=deque();pending=attempted=sent=dropped=peak=0;late=0.0
    os.set_blocking(sys.stdout.fileno(),False)
    start=time.monotonic();nominal=total/rate
    with a.path.open('rb') as src:
        while attempted<total or queue:
            now=time.monotonic()
            count=min(chunk,total-attempted)
            deadline=start+(attempted+count)/rate if count else start+nominal+a.buffer_ms/1000+.05
            if count and now>=deadline:
                late=max(late,now-deadline)
                data=src.read(count*stride)
                if len(data)!=count*stride:raise RuntimeError('Incomplete source file')
                attempted+=count
                if pending+len(data)>capacity:dropped+=len(data)
                else:
                    queue.append(memoryview(data));pending+=len(data);peak=max(peak,pending)
                continue
            if not count and now>=deadline:
                dropped+=pending;queue.clear();pending=0;break
            timeout=max(0,deadline-now)
            if queue:
                _,ready,_=select.select([],[sys.stdout.fileno()],[],timeout)
                if ready:
                    try:n=os.write(sys.stdout.fileno(),queue[0])
                    except BlockingIOError:continue
                    sent+=n;pending-=n
                    if n==len(queue[0]):queue.popleft()
                    else:queue[0]=queue[0][n:]
            elif timeout:time.sleep(timeout)
    elapsed=time.monotonic()-start
    assert sent+dropped==total*stride and not sent%stride and not dropped%stride
    report=dict(mode='fixed-clock-bounded-fifo',input_samples=total,sent_samples=sent//stride,
        dropped_samples=dropped//stride,nominal_duration_s=nominal,elapsed_s=elapsed,
        buffer_capacity_samples=capacity//stride,peak_buffer_samples=peak//stride,
        max_clock_lateness_s=late,buffer_ms=a.buffer_ms)
    print('[fixed-iq] '+json.dumps(report),file=sys.stderr)
    print(f'[iq-file] end of file: {sent//stride} samples, {elapsed:.3f} s elapsed',file=sys.stderr)


if __name__=='__main__':main()

odd - optimized dd
==================

Only works on Linux, due to using special system calls, such as
`splice, sendfile or mmap`.

```
$ make
```

Usage
-----

```
$ odd -h

odd -- optimized dd (C) 2011-2017 Sebastian Krahmer

Usage:  odd [if=F1] [of=F2] [send|mmap=N MB|cores=N] [bs=N] [skip=N] [count=N]
        [quiet] [direct] [nosync]

Switches mimic classical 'dd' behavior. You can choose copy strategies:
'send' uses sendfile(2), 'mmap' copies via mmap(2) in N Megabyte chunks.
Default is using splice(2). You can balance to 'cores' CPU cores

```

You may want to read [this paper](http://stealth.openwall.net/papers/odd.pdf) for measurement
results.


# legacy

`pingpong.cpp` is the two-core ping-pong this project grew out of: two threads
bounce a value between them through a four-slot ring and one of them times the
round trip. It is the smallest thing that measures how long a cache line takes
to move between physical cores, and it is kept buildable because every number
the bus produces is ultimately a multiple of that cost.

It has since been used as the vehicle for the code-versus-environment
experiment, since a benchmark small enough to hold in your head makes a better
subject than one with a ring buffer, a checksum and a load generator in the way.
`bench/pingpong_variants.cpp` is the version that runs seven variants of it side
by side.

Build it with the rest of the project and pass an output path:

```
./build/pingpong_legacy samples.csv
python plots/plot_run.py samples.csv
```

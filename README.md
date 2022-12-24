# FlowBundler
FlowBundler is a high speed software traffic shaping (aka, rate-limiting or pacing) scheme.

 It achieves high-efficient traffic shaping by batching packets from different flows. More details of FlowBundler can be found in our INFOCOM'23 paper "Burst can be Harmless: Achieving Line-rate Software Traffic Shaping by Inter-flow Batching".

This repo contains our kernel and userspace implementations of FlowBundler prototype.

## Contents

- `kernel/sch_dfb`:  kernel module of FlowBundler.
- `kernel/sch_eiffel`: kernel module of [Eiffel](https://saeed.github.io/eiffel/)
- `kernel/sch_carousel`: kernel module of [Carousel](https://dl.acm.org/doi/10.1145/3098822.3098852)
- `bess`: [BESS](https://span.cs.berkeley.edu/bess.html) implementation of FlowBundler

## How to Use

### Kernel

#### Requirements

- Linux 5.4.0
- Kernel headers

#### Enable FlowBundler

```bash
./kernel/sch_dfb/reenable-sch.sh DEVICE_NAME
```

where `DEVICE_NAME` is the name of network device (e.g., `eth0`) to enable FlowBundler.

#### Diable FlowBundler

``` bash
./kernel/sch_dfb/disable-sch.sh
```

#### Note

In our kernel implementation, we take advantage of [Early Departure Time (EDT) model](https://lwn.net/Articles/766564/) to achieve rate-limiting. In other words, we assume that the expected departure time of a packet has already been determined when the packet arrives at the kernel module.

Thus, to make our kernel module work correctly, one may need to explictly specify pacing rate in their applications.

For `iperf3`, one needs to set the `--fq-rate` parameter.

For customized applications, one needs to set the `SO_MAX_PACING_RATE` socket option.

### BESS

See [README](bess/README.md)

## Contact

If you have any questions, contact [Danfeng Shan](https://dfshan.github.io/).

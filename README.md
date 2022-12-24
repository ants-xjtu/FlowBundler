# FlowBundler
FlowBundler is a high speed software traffic shaping / rate-limiting / pacing scheme. It achieves high-efficient traffic shaping by batching packets from different flows. More details of FlowBundler can be found in our INFOCOM'23 paper "Burst can be Harmless: Achieving Line-rate Software Traffic Shaping by Inter-flow Batching".

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

### BESS

See [README](bess/README.md).

## Contact

If you have any questions, contact [Danfeng Shan](https://dfshan.github.io/).

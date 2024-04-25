# Environment

We implemented the **FlowBundler** algorithm based on BESS, and implemented **Carousel** and **Eiffel** together.

For more information about the installation and use of BESS, check the wiki description of the original repository [https://github.com/NetSys/bess/wiki]

# Description

BESS organizes multiple modules into pipelines, and executes each module sequentially. Therefore, we implemented FlowBundler, Carousel and Eiffel as three modules in BESS.

Rate estimation is also part of FlowBundler, we have implemented a class for rate estimation when dequeuing. As described in the paper, we also implemented a very simple called "easy-map" that does not handle hash collisions, but it is enough for FlowBundler. 

In the experiment, a large number of data packets need to be generated. In order to reduce the overhead of this part of the experiment, we also implemented an efficient packet generator

The directory structure is as follows: 

```Plain
.
└── core
    ├── modules
    │   ├── carousel.cc
    │   ├── carousel.h
    │   ├── eiffel.cc
    │   ├── eiffel.h
    │   ├── flowbundler.cc
    │   └── flowbundler.h
    └── utils
        ├── easy-map.cc
        ├── easy-map.h
        ├── flow-rate-estimator.cc
        ├── flow-rate-estimator.h
        ├── packet-generator.cc
        └── packet-generator.h
```

# How to use

## Requirements
- Python 3.9 with `protobuf`, `grpcio`, `scapy`

## Build BESS
Enter the project root directory and execute the build script

```bash
./build.py
```

## Prepare DPDK environment

The dpdk environment should be prepared in advance, the recommended dpdk version is 20.11.3. 

1. Bind network port to dpdk driver

```bash
sudo ./dpdk-devbind -b 01:00.0 vfio-pci
```

2. Set up hugepages

```bash
 sudo ./dpdk-hugepages.py --setup 2G
```


## Run test scripts

There are already written scripts for testing in the ***bessctl/conf/testing*** directory. 

1. Execute bessctl

```bash
./bessctl
```

2. Start daemon

```bash
daemon start
```

3. Run the bess script

```bash
run testing/multi_flow_flowbundler
```

4. Stop run bess script

```bash
daemon stop
```

# Braft Silent Data Corruption Sample Program

This repository provides a sample program that triggers **Silent Data Corruption (SDC)** in **Braft** using the **BFI (Bit Flip Injection) tool**, which is based on **Intel Pin**.

## References

- [Braft](https://github.com/baidu/braft)
- [Brpc](https://github.com/apache/incubator-brpc)
- [Intel Pin](https://www.intel.com/content/www/us/en/developer/articles/tool/pin-a-dynamic-binary-instrumentation-tool.html)
- [BFI tool](https://bitbucket.org/db7/bfi/src/master/)

## Usage

### 1. Install Intel Pin 

### 2. Compile Required Repositories

Compile Braft, Brpc, and BFI. Please refer to the official documentation for each repository.

### 3. Navigate to the `counter` Directory

Move to the `example/counter` directory of Braft.

```sh
cd /path/to/braft/example/counter
```
This program targets the counter application.

### 4. Compile

Compile `counter`.

```sh
make
```

### 5. Run the BFI Tool

To apply BFI and induce Silent Data Corruption, execute the following command:

```sh
./run_bfi
```

### 6. Run the Client

Next, execute the client and log each request.

```sh
./run_client.sh --log_each_request=1
```

After execution, you will see output similar to the following:

```txt
I0312 07:32:41.381088 24635 4294968832 /home/ubuntu/braft/example/counter/client.cpp:94] value=0 latency=561625
I0312 07:32:42.454969 24633 4294968832 /home/ubuntu/braft/example/counter/client.cpp:94] value=1 latency=72350
I0312 07:32:43.475967 24633 4294968832 /home/ubuntu/braft/example/counter/client.cpp:94] value=2 latency=20640
I0312 07:32:44.487631 24633 4294968832 /home/ubuntu/braft/example/counter/client.cpp:94] value=3 latency=11318
I0312 07:32:45.496752 24633 4294968832 /home/ubuntu/braft/example/counter/client.cpp:94] value=4 latency=8787
I0312 07:32:46.511448 24636 4294968832 /home/ubuntu/braft/example/counter/client.cpp:94] value=5 latency=14318
I0312 07:32:47.567475 24636 4294968832 /home/ubuntu/braft/example/counter/client.cpp:94] value=6 latency=55640
I0312 07:32:48.575239 24633 4294968832 /home/ubuntu/braft/example/counter/client.cpp:94] value=7 latency=7416
I0312 07:32:49.586480 24636 4294968832 /home/ubuntu/braft/example/counter/client.cpp:94] value=8 latency=10963
I0312 07:32:50.597390 24636 4294968832 /home/ubuntu/braft/example/counter/client.cpp:94] value=9 latency=10531
I0312 07:32:51.606112 24636 4294968832 /home/ubuntu/braft/example/counter/client.cpp:94] value=10 latency=8401
I0312 07:32:52.615777 24636 4294968832 /home/ubuntu/braft/example/counter/client.cpp:94] value=11 latency=9369
I0312 07:32:53.627535 24636 4294968832 /home/ubuntu/braft/example/counter/client.cpp:94] value=12 latency=11509
I0312 07:32:54.636525 24636 4294968832 /home/ubuntu/braft/example/counter/client.cpp:94] value=13 latency=8590
I0312 07:32:55.644628 24636 4294968832 /home/ubuntu/braft/example/counter/client.cpp:94] value=14 latency=7717
I0312 07:32:59.584028 24633 4294968832 /home/ubuntu/braft/example/counter/client.cpp:94] value=15 latency=2939000
I0312 07:33:00.596040 24633 4294968832 /home/ubuntu/braft/example/counter/client.cpp:94] value=-9223372036854775792 latency=11670
I0312 07:33:01.604810 24633 4294968832 /home/ubuntu/braft/example/counter/client.cpp:94] value=-9223372036854775791 latency=8389
I0312 07:33:02.633240 24635 4294968832 /home/ubuntu/braft/example/counter/client.cpp:94] value=-9223372036854775790 latency=28072
I0312 07:33:03.642186 24635 4294968832 /home/ubuntu/braft/example/counter/client.cpp:94] value=-9223372036854775789 latency=8479
I0312 07:33:04.650961 24635 4294968832 /home/ubuntu/braft/example/counter/client.cpp:94] value=-9223372036854775788 latency=8431
I0312 07:33:05.658791 24636 4294968832 /home/ubuntu/braft/example/counter/client.cpp:94] value=-9223372036854775787 latency=7486
I0312 07:33:06.668045 24636 4294968832 /home/ubuntu/braft/example/counter/client.cpp:94] value=-9223372036854775786 latency=8910
I0312 07:33:07.676504 24635 4294968832 /home/ubuntu/braft/example/counter/client.cpp:94] value=-9223372036854775785 latency=7925
I0312 07:33:08.684863 24635 4294968832 /home/ubuntu/braft/example/counter/client.cpp:94] value=-9223372036854775784 latency=7988
I0312 07:33:09.692614 24635 4294968832 /home/ubuntu/braft/example/counter/client.cpp:94] value=-9223372036854775783 latency=7433
I0312 07:33:10.701729 24635 4294968832 /home/ubuntu/braft/example/counter/client.cpp:94] value=-9223372036854775782 latency=8724
I0312 07:33:11.712355 24636 4294968832 /home/ubuntu/braft/example/counter/client.cpp:94] value=-9223372036854775781 latency=10339
...
```

## License

Please comply with the licenses of each referenced project.



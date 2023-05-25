
### Checkout a video demo [here](https://www.youtube.com/watch?v=-hzAsE0ymho&feature=youtu.be)

### Build

To build this project on Linux, you need to build the Capnproto WASM code objects with the following command in the `capnproto/c++` dir:

```
sudo bash -c "source ../../emsdk/emsdk_env.sh; emconfigure ./configure --host=wasm32 --disable-shared; emmake make -j6 check"
```

Next, you may compile the glue script with the following command:

```
emcc -I/usr/local/include ../capnproto/c++/.libs/libcapnp.a ../capnproto/c++/.libs/libkj.a ../capnproto/c++/.libs/libcapnp-json.a wasm.cpp --bind -sNO_DISABLE_EXCEPTION_CATCHING -sALLOW_MEMORY_GROWTH -sEXPORT_ES6 -sMODULARIZE -O3
```

Finally, copy the resultant emscripten code objects (.js and wasm .a) into the `mcap-support` package in [our Foxglove studio branch](https://github.com/jon-chuang/studio):

```
cp a.out.js ../../ui-viz/studio/packages/mcap-support/src/
cp a.out.wasm ../../ui-viz/studio/packages/mcap-support/src/
```

The branch can then import openpilot logs that have been serialized into mcap files using the scripts available in this repo.


# Test fixtures

Drop sample `.rsm` + matching `.exe` pairs here. These are committed inputs
the parser and emitter are validated against.

Expected first fixture:

- `hello.exe`  - produced from `examples/01_hello/hello.dpr`
- `hello.rsm`  - the remote-symbol file produced alongside it

Procedure to regenerate (on the Delphi build machine):

```
cd examples/01_hello
build.cmd        # invokes dcc64.exe with -V -VR -B
copy hello.exe ..\..\test\fixtures\
copy hello.rsm ..\..\test\fixtures\
```

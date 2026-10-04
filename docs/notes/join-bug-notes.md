# Join bug: leads for the fixer

Bug: `timeout:startup` on ladder step 1. The client sends its 15-byte hello to UDP 7777. The server never answers, and zero control messages reach the server world. Upstream noted that two-player play "broke" in 2025.

## Ruled out
- **GameCoordinator mode.** It turns on only when argv[3] contains "GameCoordinator" (`reborn/Init.cpp:164-166`). The runner's server argv[3] is `-nomoviestartup`, and no server log shows "Init in game coordinator mode!". With the coordinator off, `WorldControlMessageHook` welcomes any connection (`Hooks.cpp:62-71`).
- **The localhost:5000 endpoint.** `Constants::GameCoordinatorEndpoint` is used only by the client browser and login code.
- **-nullrhi.** The join fails the same way without it.

## Most likely causes, in order
1. **The world never ticks this net driver.** `InitListen` attaches the driver by writing `theWorld->NetDriver` at offset 0x128 (`Networking.cpp:88,100`). That offset comes from a hand-edited `UWorld` layout (`BB/SDK_HEADERS/Engine_classes.hpp:44431-44432`). If it is wrong, the socket is bound but `TickDispatch` never runs. That matches the evidence: the port is open and nothing reads it.
   - Test: a cdb breakpoint on `ws2_32!recvfrom` / `WSARecvFrom` on the server after "listening on port 7777".
   - Confirm the offset in Ghidra (`debugloop/ghidra/out/battleborn_functions.json`; `UWorld::Tick` is reached from 0x207e10).
2. **The wrong driver object.**
   - `GetLastOfClass<UTcpNetDriver>()` (`Networking.cpp:84`) doesn't skip `Default__` templates. Compare `Autopilot.cpp:67`, which does.
   - `FName(020724)` (`Networking.cpp:82`) is an **octal** literal, which is 8660 (`FMeshListUpdateComputeShader`). `NameDump.txt:8971` lists `TcpNetDriver` at index 20724 decimal.
   - Test: log `NetDriver->GetFullName()` in `InitListen`, and fix the literal to `20724`.
3. **Packets are received but dropped before a connection exists.**
   - The notify interface is passed as `world + 0x58` (`Networking.cpp:98`). If that isn't the real notify sub-object, the accept call goes through the wrong method table.
   - Or the client and server drivers differ. Compare `NetworkDevice=` in the server and client Engine ini files.

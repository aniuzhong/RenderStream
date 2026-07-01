# RenderStream

- [RenderStream](#renderstream)
  - [Concept](#concept)
  - [Progress](#progress)
    - [common](#common)
    - [rs-dll (RenderStream DLL)](#rs-dll-renderstream-dll)
    - [rs-agent](#rs-agent)
      - [test 🗑️](#test-️)
    - [rs-client](#rs-client)

## Concept

```mermaid
flowchart LR
    subgraph RenderStream Client Node
        rs-client
    end

    subgraph Cluster Node A Controller
        rs-agent_NodeA
        UE5.x_NodeA
        RenderStream-UE_NodeA
        renderstream.dll_NodeA
    end

    subgraph Cluster Node B Follower
        rs-agent_NodeB
        UE5.x_NodeB
        RenderStream-UE_NodeB
        renderstream.dll_NodeB
    end

rs-client -- HTTP --> rs-agent_NodeA
rs-client <-- TCP --> renderstream.dll_NodeA
rs-client -- HTTP --> rs-agent_NodeB
rs-client <-- TCP --> renderstream.dll_NodeB

rs-agent_NodeA --> UE5.x_NodeA
UE5.x_NodeA --> RenderStream-UE_NodeA
RenderStream-UE_NodeA -- LoadLibrary --> renderstream.dll_NodeA

rs-agent_NodeB --> UE5.x_NodeB
UE5.x_NodeB --> RenderStream-UE_NodeB
RenderStream-UE_NodeB -- LoadLibrary --> renderstream.dll_NodeB
```

## Progress

|||
|---|---|
| 🚧 | WIP |
| ✅ | Done |
| 🔄 | Refactoring |
| 🗑️ | Scheduled for deletion |

### common

- ✅ **`d3renderstream.h`**
- 🔄 **`d3renderstream.hpp`**

### rs-dll (RenderStream DLL)

- ✅ **`logging.h`** / **`logging.cpp`**
- 🔄 **`streams.h`** / **`streams.cpp`**
- 🔄 **`gpu.h`** / **`gpu.cpp`**
- 🔄 **`sender.h`** / **`sender.cpp`**
- 🔄 **`link.h`** / **`link.cpp`**
- 🚧 **`renderstream.cpp`**

### rs-agent

- 🔄 **`http_server.h`** / **`http_server.cpp`**
- 🔄 **`lan_announcer.h`** / **`lan_announcer.cpp`**
- 🔄 **`pipe_server.h`** / **`pipe_server.cpp`**
- 🔄 **`process_manager.h`** / **`process_manager.cpp`**

#### test 🗑️

### rs-client

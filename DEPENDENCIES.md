# Dependências

O build usa somente seis repositórios em `external/solutions`:

| Diretório | Uso | Commit fixado |
|---|---|---|
| `zstd` | compressão de saves | `82d322c4973d9e2968d94047a40892bc6d9a9bdf` |
| `blake3` | hashing e integridade | `77b257eee7da5cd608eaf6be8343d3a4c9776af2` |
| `flatbuffers` | serialização do mundo | `5761d6e67af841d15ee21bc1ce9a78ffa9cf939e` |
| `rocksdb` | armazenamento persistente | `323d915dbcaed4a7a1d8bf73389c389c08f69e03` |
| `entt` | ECS | `85c6bba014049b5de8fad49d25424df2f1f6a8c1` |
| `recast-navigation` | navegação Recast/Detour | `9f4ce64458dfae86e1239c525ddc219c4e9e06f1` |

## Preparação reproduzível

Na raiz do repositório, usando PowerShell:

```powershell
$dependencies = @(
    @{ Name='zstd'; Url='https://github.com/facebook/zstd.git'; Commit='82d322c4973d9e2968d94047a40892bc6d9a9bdf' },
    @{ Name='blake3'; Url='https://github.com/BLAKE3-team/BLAKE3.git'; Commit='77b257eee7da5cd608eaf6be8343d3a4c9776af2' },
    @{ Name='flatbuffers'; Url='https://github.com/google/flatbuffers.git'; Commit='5761d6e67af841d15ee21bc1ce9a78ffa9cf939e' },
    @{ Name='rocksdb'; Url='https://github.com/facebook/rocksdb.git'; Commit='323d915dbcaed4a7a1d8bf73389c389c08f69e03' },
    @{ Name='entt'; Url='https://github.com/skypjack/entt.git'; Commit='85c6bba014049b5de8fad49d25424df2f1f6a8c1' },
    @{ Name='recast-navigation'; Url='https://github.com/recastnavigation/recastnavigation.git'; Commit='9f4ce64458dfae86e1239c525ddc219c4e9e06f1' }
)
New-Item -ItemType Directory -Force external/solutions | Out-Null
foreach ($dependency in $dependencies) {
    $path = Join-Path external/solutions $dependency.Name
    git clone --filter=blob:none $dependency.Url $path
    git -C $path checkout --detach $dependency.Commit
}
```

Depois:

```powershell
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

O CMake baixa GLFW, GLM, vk-bootstrap, Vulkan Memory Allocator, miniaudio e
ImGui automaticamente. Bullet e Jolt já estão em `third_party`. O Vulkan SDK
deve estar instalado. Os outros projetos do catálogo interno não são necessários.

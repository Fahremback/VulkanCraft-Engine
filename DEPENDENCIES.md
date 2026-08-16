# Dependências reproduzíveis

O build atual usa somente estes 11 repositórios em `external/solutions`. Eles não são incluídos neste repositório para evitar inflar o clone, mas cada revisão é fixada abaixo.

| Diretório | Uso | Commit fixado |
|---|---|---|
| `zstd` | compressão de saves | `82d322c4973d9e2968d94047a40892bc6d9a9bdf` |
| `blake3` | hashing e integridade | `77b257eee7da5cd608eaf6be8343d3a4c9776af2` |
| `flatbuffers` | serialização do mundo | `5761d6e67af841d15ee21bc1ce9a78ffa9cf939e` |
| `rocksdb` | armazenamento persistente | `323d915dbcaed4a7a1d8bf73389c389c08f69e03` |
| `entt` | ECS | `85c6bba014049b5de8fad49d25424df2f1f6a8c1` |
| `recast-navigation` | navegação Recast/Detour | `9f4ce64458dfae86e1239c525ddc219c4e9e06f1` |
| `fast-wfc` | geração de estruturas WFC | `0edfccf8f354da79b00ebfea7b1dbee5271e9c1f` |
| `delaunator-cpp` | grafo viário e parcelamento | `c1521f6e879881232dcddabd6c2ddb6187e8714b` |
| `earcut-hpp` | triangulação de polígonos | `f25bc765e3084583b7350080319c29ad87bf5857` |
| `meshoptimizer` | otimização e simplificação de meshes | `97bbdce4716f6257c9527b051515136882f33e79` |
| `xatlas` | geração de UVs | `f700c7790aaa030e794b52ba7791a05c085faf0c` |

## Preparação no PowerShell

Execute na raiz do repositório:

```powershell
$dependencies = @(
    @{ Name='zstd'; Url='https://github.com/facebook/zstd.git'; Commit='82d322c4973d9e2968d94047a40892bc6d9a9bdf' },
    @{ Name='blake3'; Url='https://github.com/BLAKE3-team/BLAKE3.git'; Commit='77b257eee7da5cd608eaf6be8343d3a4c9776af2' },
    @{ Name='flatbuffers'; Url='https://github.com/google/flatbuffers.git'; Commit='5761d6e67af841d15ee21bc1ce9a78ffa9cf939e' },
    @{ Name='rocksdb'; Url='https://github.com/facebook/rocksdb.git'; Commit='323d915dbcaed4a7a1d8bf73389c389c08f69e03' },
    @{ Name='entt'; Url='https://github.com/skypjack/entt.git'; Commit='85c6bba014049b5de8fad49d25424df2f1f6a8c1' },
    @{ Name='recast-navigation'; Url='https://github.com/recastnavigation/recastnavigation.git'; Commit='9f4ce64458dfae86e1239c525ddc219c4e9e06f1' },
    @{ Name='fast-wfc'; Url='https://github.com/math-fehr/fast-wfc.git'; Commit='0edfccf8f354da79b00ebfea7b1dbee5271e9c1f' },
    @{ Name='delaunator-cpp'; Url='https://github.com/delfrrr/delaunator-cpp.git'; Commit='c1521f6e879881232dcddabd6c2ddb6187e8714b' },
    @{ Name='earcut-hpp'; Url='https://github.com/mapbox/earcut.hpp.git'; Commit='f25bc765e3084583b7350080319c29ad87bf5857' },
    @{ Name='meshoptimizer'; Url='https://github.com/zeux/meshoptimizer.git'; Commit='97bbdce4716f6257c9527b051515136882f33e79' },
    @{ Name='xatlas'; Url='https://github.com/jpcy/xatlas.git'; Commit='f700c7790aaa030e794b52ba7791a05c085faf0c' }
)

New-Item -ItemType Directory -Force external/solutions | Out-Null
foreach ($dependency in $dependencies) {
    $path = Join-Path external/solutions $dependency.Name
    if (-not (Test-Path $path)) {
        git clone --filter=blob:none $dependency.Url $path
    }
    git -C $path fetch --depth 1 origin $dependency.Commit
    git -C $path checkout --detach $dependency.Commit
}
```

## Build e testes

```powershell
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

O CMake baixa GLFW, GLM, vk-bootstrap, Vulkan Memory Allocator, miniaudio e ImGui automaticamente. Bullet, Jolt e FastNoiseLite estão em `third_party`. O Vulkan SDK deve estar instalado.

O diretório `external/` permanece ignorado pelo Git. Nenhum dos demais projetos do catálogo interno é necessário para compilar esta revisão.

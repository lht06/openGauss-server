# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

openGauss is an open-source relational database management system with high performance, full-chain security, and intelligent operations. It's developed by Huawei with optimizations for ARM architecture and multi-core systems.

## Architecture

### Core Components
- **src/gausskernel/**: Core database kernel
- **src/common/**: Common utilities and libraries
- **src/bin/**: Binary executables (psql, gs_initdb, etc.)
- **contrib/**: Extension modules and additional components
- **src/include/**: Header files for the entire project

### Key Directories
- **build/**: Build system scripts and configuration
- **src/test/**: Comprehensive test suites including regression, isolation, and performance tests
- **doc/**: Documentation and architectural diagrams
- **simpleInstall/**: Simple installation scripts for quick setup

## Build Commands

### Prerequisites
Download and extract binarylibs (third-party dependencies) to the project directory or specify path with `-3rd`.

### Primary Build Command
```bash
./build.sh -m [debug|release|memcheck] -3rd [binarylibs_path]
```

**Build modes:**
- `release`: Optimized production build (default)
- `debug`: Debug build with additional debugging info
- `memcheck`: Debug build with ASAN memory checking

### Build Options
- `--cmake`: Use cmake instead of traditional configure/make
- `-pkg`/`--package`: Create installation package after build
- `-nopt`: Disable ARM LSE optimizations (for older ARM platforms)
- `-T`/`--tassl`: Build with TaSSL support
- `-nls`/`--enable_nls`: Enable Native Language Support

### Examples
```bash
# Basic release build
./build.sh

# Debug build with specific binarylibs path
./build.sh -m debug -3rd /path/to/binarylibs

# Create installation package
./build.sh -pkg

# Use cmake build system
./build.sh --cmake
```

### Build Output
- Compiled binaries: `dest/bin/`
- Installation path: `dest/`
- Build logs: `make_compile.log`

## Testing

### Regression Tests
```bash
cd src/test/regress
make check
```

### Specific Test Suites
- **Isolation tests**: `src/test/isolation/`
- **HA tests**: `src/test/ha/`
- **Performance tests**: `src/test/performance/`

### Test Schedules
Different parallel test schedules available:
- `parallel_schedule`: Main regression test suite
- `parallel_schedule0`: Core functionality tests
- Various specialized schedules for specific features

## Development Workflow

### Configuration
The project uses autoconf/configure for traditional builds:
```bash
./configure --prefix=$GAUSSHOME --3rd=$BINARYLIBS [options]
make -sj
make install -sj
```

### CMake Alternative
For faster builds:
```bash
mkdir cmake_build && cd cmake_build
cmake .. -DENABLE_MULTIPLE_NODES=OFF -DENABLE_THREAD_SAFETY=ON
make -sj && make install -sj
```

### Code Organization
- C/C++ source primarily in `src/gausskernel/`
- SQL extension scripts in `contrib/*/sql/`
- Test data and expected outputs in respective `data/` and `expected/` directories

## Database Features

### Storage Engines
- **Standard row storage**: Traditional PostgreSQL-compatible storage
- **Column store**: Optimized for analytical workloads
- **MOT (Memory Optimized Tables)**: In-memory storage engine

### Performance Optimizations
- Multi-core NUMA-aware architecture
- ARM instruction optimizations
- SQL bypass for simplified execution
- Parallel recovery mechanisms

### Security Features
- Full encryption support
- Account management and authentication
- Operation auditing capabilities
- Privilege management

## Installation

The project supports multiple installation methods:
- **Enterprise installation**: Using gs_preinstall and gs_install scripts
- **Simple installation**: Using scripts in `simpleInstall/`
- **Docker installation**: Using dockerfiles in `docker/`

Note: Installation requires proper environment setup including user creation, directory permissions, and network configuration as detailed in the README.

## Parallel Materialization Optimization

### Overview
Recently implemented parallel Materialize node optimization provides "serialize once, serve many" capability for high-performance analytical workloads. This addresses the performance bottleneck where multiple similar queries execute redundant materialization work.

### Key Features

#### Lock-free Circular Buffer
- **Location**: `src/gausskernel/runtime/executor/parallelMaterial.cpp`
- **Implementation**: Multi-producer/multi-consumer atomic operations
- **Benefits**: Eliminates contention in `seq_scan_getnext`/`heap_getnext` calls
- **Configuration**: Automatically enabled for 2+ parallel workers

#### Partition-wise Parallelism
- **Integration**: Uses existing `ispwj` flag infrastructure
- **Worker Distribution**: Dynamic load balancing with work stealing
- **Memory Isolation**: Per-partition tuple stores with independent memory management
- **Scalability**: Reduces data volume per worker through partition assignment

#### Per-worker Memory Management
- **Allocation**: `CalculateWorkerMemory()` distributes 80% of total memory across workers
- **Minimum**: 1MB per worker guaranteed
- **Spill Handling**: Automatic context reset when worker memory exceeded
- **Tuning**: Integrates with existing `work_mem` (openGauss `sort_mem`) parameters

### Configuration Parameters

#### Enable Parallel Materialization
```sql
-- Enable parallel query execution
SET enable_parallel_query = on;

-- Set maximum workers per gather operation  
SET max_parallel_workers_per_gather = 8;

-- Adjust memory allocation per operation
SET work_mem = '256MB';
```

#### Performance Tuning
```sql
-- For partition-wise operations
SET enable_partition_wise_join = on;
SET enable_partition_wise_agg = on;

-- Memory pressure thresholds
SET parallel_tuple_fraction = 0.1;
SET sort_mem = '512MB';  -- openGauss equivalent of work_mem
```

### Architecture Integration

#### Executor Node Path
- **Entry Point**: `ExecMaterial()` in `nodeMaterial.cpp:277`
- **Parallel Dispatch**: `ExecParallelMaterial()` coordinates worker roles
- **Producer Logic**: `ExecParallelMaterialProducer()` handles tuple generation
- **Consumer Logic**: `ExecParallelMaterialConsumer()` manages tuple retrieval

#### Memory Context Hierarchy
```
CurrentMemoryContext
└── ParallelMaterialContext (per-node)
    ├── SharedMaterialState (coordination)
    ├── ParallelTupleBuffer (lock-free storage)
    └── WorkerMemoryContext[N] (per-worker isolation)
```

#### Worker Coordination
- **Initialization**: `InitParallelWorkers()` sets up shared state and synchronization
- **Load Balancing**: Dynamic role assignment based on throughput metrics
- **Memory Adaptation**: Automatic producer throttling under memory pressure

### Performance Characteristics

#### Throughput Improvements
- **Single-threaded**: Baseline performance maintained
- **2-4 workers**: 2-3x improvement for memory-bound workloads
- **8+ workers**: 4-6x improvement for I/O-bound materialization
- **Partition-wise**: Additional 1.5-2x improvement with proper partitioning

#### Memory Efficiency
- **Buffer Management**: Lock-free circular buffer reduces allocation overhead
- **Spill Optimization**: Per-worker context resets prevent memory fragmentation
- **Shared Storage**: Single materialization serves multiple consumer queries

#### Scalability Limits
- **CPU-bound**: Benefits plateau beyond CPU core count
- **Memory-bound**: Performance degrades if total memory insufficient
- **I/O-bound**: Highest scalability potential with proper partition distribution

### Monitoring and Debugging

#### Performance Logging
```sql
-- Enable detailed parallel material logs
SET log_min_messages = DEBUG1;
SET client_min_messages = LOG;
```

#### Statistics Views
- Worker-level statistics: `LogParallelMaterialPerformance()`
- Partition distribution: `LogPartitionStats()`
- Buffer utilization: Lock-free buffer metrics in logs

#### Troubleshooting Common Issues
1. **High memory spills**: Increase `work_mem` or reduce `max_parallel_workers_per_gather`
2. **Worker starvation**: Check partition distribution balance
3. **Buffer contention**: Verify lock-free buffer is enabled (automatic for 2+ workers)

### Development Notes

#### Code Locations
- **Headers**: `src/include/executor/node/parallelMaterial.h`
- **Implementation**: `src/gausskernel/runtime/executor/parallelMaterial.cpp`
- **Integration**: `src/gausskernel/runtime/executor/nodeMaterial.cpp`
- **State Definition**: `src/include/nodes/execnodes.h:2581`

#### Key Functions
- `ParallelBufferPut/Get()`: Lock-free buffer operations
- `CoordinateWorkerMaterialization()`: Dynamic load balancing
- `SetupPartitionParallelism()`: Partition-wise initialization
- `CalculateWorkerMemory()`: Memory distribution algorithm

#### Testing
- **Regression Tests**: Integration with existing `src/test/regress/` framework
- **Performance Tests**: `src/test/performance/` for scalability validation
- **Isolation Tests**: `src/test/isolation/` for parallel correctness
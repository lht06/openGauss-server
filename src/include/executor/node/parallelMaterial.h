/* -------------------------------------------------------------------------
 *
 * parallelMaterial.h
 *	  Parallel Materialization support for openGauss
 *
 * This file defines structures and functions for parallel materialization,
 * implementing "serialize once, serve many" optimization for Material nodes.
 *
 * Portions Copyright (c) 2024, openGauss Contributors
 * 
 * src/include/executor/node/parallelMaterial.h
 *
 * -------------------------------------------------------------------------
 */
#ifndef PARALLEL_MATERIAL_H
#define PARALLEL_MATERIAL_H

#include "postgres.h"
#include "storage/lwlock.h"
#include "storage/condition_variable.h"
#include "storage/spin.h"
#include "utils/atomic.h"

/* Forward declarations */
struct MaterialState;
struct TupleTableSlot;
struct Tuplestorestate;

/*
 * Lock-free circular buffer for parallel tuple storage
 * Uses atomic operations for high-performance multi-producer/multi-consumer access
 */
typedef struct ParallelTupleBuffer {
    /* Buffer metadata */
    size_t buffer_size;                 /* Size of the buffer (power of 2) */
    size_t buffer_mask;                 /* Mask for fast modulo operation */
    
    /* Atomic counters for lock-free operation */
    pg_atomic_uint64 write_pos;         /* Write position (producer) */
    pg_atomic_uint64 read_pos;          /* Read position (consumer) */
    pg_atomic_uint32 active_producers;  /* Number of active producer workers */
    pg_atomic_uint32 active_consumers;  /* Number of active consumer workers */
    
    /* Memory management */
    MemoryContext buffer_context;       /* Memory context for buffer allocation */
    size_t tuple_slot_size;            /* Size of each tuple slot */
    
    /* Buffer array - must be last field for variable length */
    char buffer[];                      /* Actual tuple storage */
} ParallelTupleBuffer;

/*
 * Parallel worker information for Material node
 */
typedef struct ParallelWorkerInfo {
    int worker_id;                      /* Unique worker identifier */
    int total_workers;                  /* Total number of parallel workers */
    
    /* Worker-specific memory allocation */
    int64 worker_mem_kb;               /* Memory allocated to this worker */
    MemoryContext worker_context;       /* Worker-specific memory context */
    
    /* Synchronization primitives */
    LWLock* coordination_lock;          /* Lock for worker coordination */
    ConditionVariable* materialized_cv; /* CV for materialization completion */
    
    /* Worker state */
    bool is_producer;                   /* True if this worker produces tuples */
    bool is_consumer;                   /* True if this worker consumes tuples */
    bool materialization_done;          /* Materialization completed flag */
    
    /* Statistics */
    uint64 tuples_produced;            /* Number of tuples produced by worker */
    uint64 tuples_consumed;            /* Number of tuples consumed by worker */
    uint64 memory_spills;              /* Number of memory spills to disk */
} ParallelWorkerInfo;

/*
 * Shared memory state for parallel materialization
 */
typedef struct SharedMaterialState {
    /* Shared buffer */
    ParallelTupleBuffer* shared_buffer;
    
    /* Global synchronization */
    LWLock* global_lock;
    ConditionVariable* all_done_cv;
    
    /* Global state flags */
    pg_atomic_uint32 materialization_state; /* 0=starting, 1=running, 2=done */
    pg_atomic_uint32 total_tuples;          /* Total tuples materialized */
    pg_atomic_uint32 error_state;           /* Error condition flag */
    
    /* Memory management */
    int64 total_memory_kb;              /* Total memory allocated */
    int64 spill_threshold_kb;           /* Threshold for spilling to disk */
    
    /* Worker coordination */
    int num_workers;
    ParallelWorkerInfo workers[];       /* Variable length array */
} SharedMaterialState;

/*
 * Partition-wise parallel state
 */
typedef struct PartitionParallelState {
    int num_partitions;                 /* Number of partitions */
    int* partition_worker_map;          /* Maps partition to worker */
    struct Tuplestorestate** partition_stores; /* Per-partition tuple stores */
    bool* partition_done;               /* Completion status per partition */
} PartitionParallelState;

/* Function declarations */

/* Buffer management */
extern ParallelTupleBuffer* CreateParallelTupleBuffer(size_t buffer_size, 
                                                     size_t tuple_size,
                                                     MemoryContext context);
extern void DestroyParallelTupleBuffer(ParallelTupleBuffer* buffer);

/* Lock-free buffer operations */
extern bool ParallelBufferPut(ParallelTupleBuffer* buffer, 
                             struct TupleTableSlot* slot);
extern bool ParallelBufferGet(ParallelTupleBuffer* buffer, 
                             struct TupleTableSlot* slot);

/* Worker coordination */
extern void InitParallelWorkers(struct MaterialState* node, int num_workers);
extern void ShutdownParallelWorkers(struct MaterialState* node);
extern void CoordinateWorkerMaterialization(struct MaterialState* node);

/* Memory management */
extern int64 CalculateWorkerMemory(int64 total_memory_kb, int num_workers);
extern void SetupWorkerMemoryContext(ParallelWorkerInfo* worker, int64 memory_kb);

/* Partition-wise parallelism */
extern PartitionParallelState* SetupPartitionParallelism(int num_partitions, 
                                                         int num_workers);
extern void ShutdownPartitionParallelism(PartitionParallelState* state);
extern int GetPartitionForWorker(PartitionParallelState* state, int worker_id, int num_workers);
extern void MarkPartitionDone(PartitionParallelState* state, int partition_id);
extern bool IsAllPartitionsDone(PartitionParallelState* state);
extern struct Tuplestorestate* GetPartitionTupleStore(PartitionParallelState* state, 
                                                     int partition_id, int64 work_mem, 
                                                     int64 max_mem, int plan_node_id);
extern int64 EstimatePartitionMemory(int num_partitions, int64 total_memory_kb);
extern void LogPartitionStats(PartitionParallelState* state);

/* Performance monitoring */
extern void UpdateMaterialStats(struct MaterialState* node);
extern void LogParallelMaterialPerformance(struct MaterialState* node);

#endif /* PARALLEL_MATERIAL_H */
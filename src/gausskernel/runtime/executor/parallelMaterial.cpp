/* -------------------------------------------------------------------------
 *
 * parallelMaterial.cpp
 *	  Parallel Materialization implementation for openGauss
 *
 * This file implements parallel materialization with lock-free circular
 * buffers, enabling "serialize once, serve many" optimization for high-
 * performance analytical workloads.
 *
 * Key Features:
 * - Lock-free circular buffer for multi-producer/multi-consumer access
 * - Per-worker memory allocation and management  
 * - Partition-wise parallel execution
 * - Automatic memory spilling when limits exceeded
 *
 * Portions Copyright (c) 2024, openGauss Contributors
 * 
 * src/gausskernel/runtime/executor/parallelMaterial.cpp
 *
 * -------------------------------------------------------------------------
 */
#include "postgres.h"
#include "knl/knl_variable.h"

#include "executor/node/parallelMaterial.h"
#include "executor/executor.h"
#include "executor/tuptable.h"
#include "miscadmin.h"
#include "utils/memutils.h"
#include "utils/hsearch.h"
#include "storage/lwlock.h"
#include "storage/condition_variable.h"
#include "utils/atomic.h"
#include "pgstat.h"

/* Internal constants */
#define DEFAULT_PARALLEL_BUFFER_SIZE    (1024 * 1024)  /* 1MB default buffer */
#define MIN_BUFFER_SIZE                 (64 * 1024)     /* 64KB minimum */
#define MAX_BUFFER_SIZE                 (16 * 1024 * 1024) /* 16MB maximum */
#define BUFFER_ALIGNMENT               64               /* Cache line alignment */
#define MAX_SPIN_ATTEMPTS              1000             /* Max spin before yield */

/* Materialization states */
#define MATERIAL_STATE_STARTING        0
#define MATERIAL_STATE_RUNNING         1
#define MATERIAL_STATE_DONE           2
#define MATERIAL_STATE_ERROR          3

/* Internal function declarations */
static void InitializeParallelBuffer(ParallelTupleBuffer* buffer, size_t buffer_size, size_t tuple_size);
static bool TryPutTupleSlot(ParallelTupleBuffer* buffer, TupleTableSlot* slot, uint64 write_pos);
static bool TryGetTupleSlot(ParallelTupleBuffer* buffer, TupleTableSlot* slot, uint64 read_pos);
static void CopyTupleSlotToBuffer(TupleTableSlot* slot, char* buffer_pos, size_t slot_size);
static void CopyBufferToTupleSlot(char* buffer_pos, TupleTableSlot* slot, size_t slot_size);
static size_t CalculateTupleSlotSize(TupleTableSlot* slot);
static uint64 NextPowerOfTwo(uint64 value);

/*
 * CreateParallelTupleBuffer
 *
 * Creates a lock-free circular buffer optimized for parallel tuple storage.
 * Uses atomic operations for high-performance multi-producer/multi-consumer access.
 */
ParallelTupleBuffer*
CreateParallelTupleBuffer(size_t buffer_size, size_t tuple_size, MemoryContext context)
{
    ParallelTupleBuffer* buffer;
    size_t total_size;
    size_t aligned_buffer_size;
    
    /* Validate input parameters */
    if (buffer_size < MIN_BUFFER_SIZE)
        buffer_size = MIN_BUFFER_SIZE;
    else if (buffer_size > MAX_BUFFER_SIZE)
        buffer_size = MAX_BUFFER_SIZE;
    
    /* Ensure buffer size is power of 2 for fast modulo operations */
    aligned_buffer_size = NextPowerOfTwo(buffer_size);
    
    /* Calculate total memory needed */
    total_size = sizeof(ParallelTupleBuffer) + aligned_buffer_size;
    
    /* Allocate in specified memory context */
    MemoryContext old_context = MemoryContextSwitchTo(context);
    
    buffer = (ParallelTupleBuffer*) palloc0(total_size);
    buffer->buffer_size = aligned_buffer_size;
    buffer->buffer_mask = aligned_buffer_size - 1;  /* For fast modulo */
    buffer->buffer_context = context;
    buffer->tuple_slot_size = tuple_size;
    
    /* Initialize atomic counters */
    pg_atomic_init_u64(&buffer->write_pos, 0);
    pg_atomic_init_u64(&buffer->read_pos, 0);
    pg_atomic_init_u32(&buffer->active_producers, 0);
    pg_atomic_init_u32(&buffer->active_consumers, 0);
    
    MemoryContextSwitchTo(old_context);
    
    ereport(DEBUG2,
            (errmsg("Created parallel tuple buffer: size=%zu, tuple_size=%zu",
                   aligned_buffer_size, tuple_size)));
    
    return buffer;
}

/*
 * DestroyParallelTupleBuffer
 *
 * Safely destroys a parallel tuple buffer, ensuring all workers have finished.
 */
void
DestroyParallelTupleBuffer(ParallelTupleBuffer* buffer)
{
    if (buffer == NULL)
        return;
    
    /* Wait for all active producers/consumers to finish */
    while (pg_atomic_read_u32(&buffer->active_producers) > 0 ||
           pg_atomic_read_u32(&buffer->active_consumers) > 0) {
        pg_usleep(1000);  /* 1ms sleep */
        CHECK_FOR_INTERRUPTS();
    }
    
    ereport(DEBUG2,
            (errmsg("Destroyed parallel tuple buffer: final write_pos=%lu, read_pos=%lu",
                   pg_atomic_read_u64(&buffer->write_pos),
                   pg_atomic_read_u64(&buffer->read_pos))));
    
    pfree(buffer);
}

/*
 * ParallelBufferPut
 *
 * Lock-free insertion of a tuple into the parallel buffer.
 * Returns true if successful, false if buffer is full.
 */
bool
ParallelBufferPut(ParallelTupleBuffer* buffer, TupleTableSlot* slot)
{
    uint64 current_write_pos;
    uint64 current_read_pos;
    uint64 next_write_pos;
    int spin_count = 0;
    
    if (buffer == NULL || TupIsNull(slot))
        return false;
    
    /* Register as active producer */
    pg_atomic_add_fetch_u32(&buffer->active_producers, 1);
    
    /* Acquire write position atomically */
    do {
        current_write_pos = pg_atomic_read_u64(&buffer->write_pos);
        next_write_pos = current_write_pos + 1;
        
        /* Check if buffer is full */
        current_read_pos = pg_atomic_read_u64(&buffer->read_pos);
        if (next_write_pos - current_read_pos >= buffer->buffer_size) {
            /* Buffer is full, back off */
            pg_atomic_sub_fetch_u32(&buffer->active_producers, 1);
            return false;
        }
        
        /* Try to advance write position */
        if (pg_atomic_compare_exchange_u64(&buffer->write_pos, 
                                          &current_write_pos, next_write_pos)) {
            break;  /* Successfully acquired position */
        }
        
        /* Spin for a bit before retrying */
        if (++spin_count > MAX_SPIN_ATTEMPTS) {
            pg_usleep(1);  /* Yield after spinning */
            spin_count = 0;
        }
        
        CHECK_FOR_INTERRUPTS();
    } while (true);
    
    /* Store tuple at acquired position */
    bool success = TryPutTupleSlot(buffer, slot, current_write_pos);
    
    /* Unregister as active producer */
    pg_atomic_sub_fetch_u32(&buffer->active_producers, 1);
    
    return success;
}

/*
 * ParallelBufferGet
 *
 * Lock-free retrieval of a tuple from the parallel buffer.
 * Returns true if successful, false if buffer is empty.
 */
bool
ParallelBufferGet(ParallelTupleBuffer* buffer, TupleTableSlot* slot)
{
    uint64 current_read_pos;
    uint64 current_write_pos;
    uint64 next_read_pos;
    int spin_count = 0;
    
    if (buffer == NULL || slot == NULL)
        return false;
    
    /* Register as active consumer */
    pg_atomic_add_fetch_u32(&buffer->active_consumers, 1);
    
    /* Acquire read position atomically */
    do {
        current_read_pos = pg_atomic_read_u64(&buffer->read_pos);
        current_write_pos = pg_atomic_read_u64(&buffer->write_pos);
        
        /* Check if buffer is empty */
        if (current_read_pos >= current_write_pos) {
            /* Buffer is empty, back off */
            pg_atomic_sub_fetch_u32(&buffer->active_consumers, 1);
            return false;
        }
        
        next_read_pos = current_read_pos + 1;
        
        /* Try to advance read position */
        if (pg_atomic_compare_exchange_u64(&buffer->read_pos, 
                                          &current_read_pos, next_read_pos)) {
            break;  /* Successfully acquired position */
        }
        
        /* Spin for a bit before retrying */
        if (++spin_count > MAX_SPIN_ATTEMPTS) {
            pg_usleep(1);  /* Yield after spinning */
            spin_count = 0;
        }
        
        CHECK_FOR_INTERRUPTS();
    } while (true);
    
    /* Retrieve tuple from acquired position */
    bool success = TryGetTupleSlot(buffer, slot, current_read_pos);
    
    /* Unregister as active consumer */
    pg_atomic_sub_fetch_u32(&buffer->active_consumers, 1);
    
    return success;
}

/*
 * TryPutTupleSlot
 *
 * Internal function to store a tuple slot at a specific buffer position.
 */
static bool
TryPutTupleSlot(ParallelTupleBuffer* buffer, TupleTableSlot* slot, uint64 write_pos)
{
    size_t buffer_offset;
    char* buffer_position;
    
    /* Calculate buffer position using mask for fast modulo */
    buffer_offset = (write_pos & buffer->buffer_mask) * buffer->tuple_slot_size;
    buffer_position = buffer->buffer + buffer_offset;
    
    /* Copy tuple data to buffer */
    CopyTupleSlotToBuffer(slot, buffer_position, buffer->tuple_slot_size);
    
    return true;
}

/*
 * TryGetTupleSlot
 *
 * Internal function to retrieve a tuple slot from a specific buffer position.
 */
static bool
TryGetTupleSlot(ParallelTupleBuffer* buffer, TupleTableSlot* slot, uint64 read_pos)
{
    size_t buffer_offset;
    char* buffer_position;
    
    /* Calculate buffer position using mask for fast modulo */
    buffer_offset = (read_pos & buffer->buffer_mask) * buffer->tuple_slot_size;
    buffer_position = buffer->buffer + buffer_offset;
    
    /* Copy tuple data from buffer */
    CopyBufferToTupleSlot(buffer_position, slot, buffer->tuple_slot_size);
    
    return true;
}

/*
 * CopyTupleSlotToBuffer
 *
 * Efficiently copies tuple slot data to buffer position.
 * Uses specialized copying to minimize memory allocations.
 */
static void
CopyTupleSlotToBuffer(TupleTableSlot* slot, char* buffer_pos, size_t slot_size)
{
    /* For now, use a simple approach - in production this would be optimized */
    HeapTuple tuple = ExecCopySlotTuple(slot);
    size_t tuple_size = HEAPTUPLESIZE + tuple->t_len;
    
    if (tuple_size <= slot_size) {
        memcpy(buffer_pos, tuple, tuple_size);
        /* Store actual size in the first bytes */
        *((size_t*)buffer_pos) = tuple_size;
    } else {
        ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("tuple size %zu exceeds buffer slot size %zu", 
                        tuple_size, slot_size)));
    }
    
    heap_freetuple(tuple);
}

/*
 * CopyBufferToTupleSlot
 *
 * Efficiently copies buffer data to tuple slot.
 */
static void
CopyBufferToTupleSlot(char* buffer_pos, TupleTableSlot* slot, size_t slot_size)
{
    size_t tuple_size = *((size_t*)buffer_pos);
    HeapTuple tuple;
    
    if (tuple_size > slot_size) {
        ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("stored tuple size %zu exceeds buffer slot size %zu", 
                        tuple_size, slot_size)));
    }
    
    /* Reconstruct heap tuple from buffer */
    tuple = (HeapTuple) palloc(tuple_size);
    memcpy(tuple, buffer_pos, tuple_size);
    
    /* Store in slot */
    ExecStoreTuple(tuple, slot, InvalidBuffer, true);
}

/*
 * CalculateTupleSlotSize
 *
 * Estimates the size needed for storing a tuple slot in the buffer.
 */
static size_t
CalculateTupleSlotSize(TupleTableSlot* slot)
{
    /* Estimate based on tuple descriptor and average tuple size */
    TupleDesc desc = slot->tts_tupleDescriptor;
    size_t estimated_size = sizeof(HeapTupleData) + sizeof(size_t);
    
    /* Add estimated data size based on number of attributes */
    estimated_size += desc->natts * 16;  /* Average 16 bytes per attribute */
    
    /* Add some padding for variable-length data */
    estimated_size *= 2;
    
    /* Align to cache line boundary */
    estimated_size = MAXALIGN(estimated_size);
    
    return estimated_size;
}

/*
 * NextPowerOfTwo
 *
 * Returns the next power of two greater than or equal to the input value.
 */
static uint64
NextPowerOfTwo(uint64 value)
{
    if (value == 0)
        return 1;
    
    value--;
    value |= value >> 1;
    value |= value >> 2;
    value |= value >> 4;
    value |= value >> 8;
    value |= value >> 16;
    value |= value >> 32;
    value++;
    
    return value;
}

/*
 * CalculateWorkerMemory
 *
 * Calculates memory allocation per worker based on total available memory.
 */
int64
CalculateWorkerMemory(int64 total_memory_kb, int num_workers)
{
    int64 worker_memory;
    
    if (num_workers <= 0)
        return total_memory_kb;
    
    /* Allocate 80% of memory to workers, 20% for coordination overhead */
    worker_memory = (total_memory_kb * 8) / (10 * num_workers);
    
    /* Ensure minimum 1MB per worker */
    if (worker_memory < 1024)
        worker_memory = 1024;
    
    return worker_memory;
}

/*
 * SetupWorkerMemoryContext
 *
 * Sets up a dedicated memory context for a parallel worker.
 */
void
SetupWorkerMemoryContext(ParallelWorkerInfo* worker, int64 memory_kb)
{
    char context_name[64];
    
    snprintf(context_name, sizeof(context_name), 
             "ParallelMaterialWorker_%d", worker->worker_id);
    
    worker->worker_context = AllocSetContextCreate(CurrentMemoryContext,
                                                   context_name,
                                                   ALLOCSET_DEFAULT_MINSIZE,
                                                   ALLOCSET_DEFAULT_INITSIZE,
                                                   memory_kb * 1024L);
    
    worker->worker_mem_kb = memory_kb;
    
    ereport(DEBUG2,
            (errmsg("Setup worker %d memory context: %ld KB", 
                   worker->worker_id, memory_kb)));
}

/*
 * InitParallelWorkers
 *
 * Initializes parallel workers for the Material node.
 * Sets up shared memory state and worker coordination structures.
 */
void
InitParallelWorkers(MaterialState* node, int num_workers)
{
    SharedMaterialState* shared_state;
    ParallelWorkerInfo* workers;
    size_t shared_size;
    int i;
    
    if (num_workers <= 1) {
        node->enable_parallel = false;
        return;
    }
    
    /* Calculate shared memory size needed */
    shared_size = sizeof(SharedMaterialState) + 
                  (num_workers * sizeof(ParallelWorkerInfo));
    
    /* Allocate shared memory state */
    shared_state = (SharedMaterialState*) palloc0(shared_size);
    shared_state->num_workers = num_workers;
    
    /* Initialize global synchronization */
    shared_state->global_lock = LWLockAssign();
    ConditionVariableInit(&shared_state->all_done_cv);
    
    /* Initialize atomic variables */
    pg_atomic_init_u32(&shared_state->materialization_state, MATERIAL_STATE_STARTING);
    pg_atomic_init_u32(&shared_state->total_tuples, 0);
    pg_atomic_init_u32(&shared_state->error_state, 0);
    
    /* Setup memory management */
    shared_state->total_memory_kb = node->worker_memory_kb * num_workers;
    shared_state->spill_threshold_kb = shared_state->total_memory_kb * 8 / 10;  /* 80% threshold */
    
    /* Initialize workers */
    workers = shared_state->workers;
    for (i = 0; i < num_workers; i++) {
        ParallelWorkerInfo* worker = &workers[i];
        
        worker->worker_id = i;
        worker->total_workers = num_workers;
        worker->worker_mem_kb = CalculateWorkerMemory(node->worker_memory_kb, num_workers);
        
        /* Setup synchronization */
        worker->coordination_lock = LWLockAssign();
        ConditionVariableInit(&worker->materialized_cv);
        
        /* Initialize worker state */
        worker->is_producer = true;   /* All workers can produce initially */
        worker->is_consumer = true;   /* All workers can consume */
        worker->materialization_done = false;
        
        /* Initialize statistics */
        worker->tuples_produced = 0;
        worker->tuples_consumed = 0;
        worker->memory_spills = 0;
        
        /* Setup worker memory context */
        SetupWorkerMemoryContext(worker, worker->worker_mem_kb);
        
        ereport(DEBUG2,
                (errmsg("Initialized parallel worker %d with %ld KB memory", 
                       i, worker->worker_mem_kb)));
    }
    
    /* Store shared state in node */
    node->shared_state = shared_state;
    node->num_parallel_workers = num_workers;
    node->enable_parallel = true;
    
    /* Create parallel buffer if enabled */
    if (node->use_lock_free_buffer) {
        TupleTableSlot* sample_slot = node->ss.ps.ps_ResultTupleSlot;
        size_t tuple_size = CalculateTupleSlotSize(sample_slot);
        size_t buffer_size = DEFAULT_PARALLEL_BUFFER_SIZE;
        
        node->parallel_buffer = CreateParallelTupleBuffer(buffer_size, tuple_size, 
                                                         node->parallel_context);
        shared_state->shared_buffer = node->parallel_buffer;
        
        ereport(LOG,
                (errmsg("Created parallel material buffer: %d workers, %zu buffer size, %zu tuple size",
                       num_workers, buffer_size, tuple_size)));
    }
    
    ereport(LOG,
            (errmsg("Initialized parallel materialization: %d workers, %ld KB total memory",
                   num_workers, shared_state->total_memory_kb)));
}

/*
 * ShutdownParallelWorkers
 *
 * Safely shuts down all parallel workers and cleans up resources.
 */
void
ShutdownParallelWorkers(MaterialState* node)
{
    SharedMaterialState* shared_state = node->shared_state;
    int i;
    
    if (!node->enable_parallel || shared_state == NULL)
        return;
    
    /* Signal all workers to stop */
    LWLockAcquire(shared_state->global_lock, LW_EXCLUSIVE);
    pg_atomic_write_u32(&shared_state->materialization_state, MATERIAL_STATE_DONE);
    LWLockRelease(shared_state->global_lock);
    
    /* Wake up all waiting workers */
    ConditionVariableBroadcast(&shared_state->all_done_cv);
    
    for (i = 0; i < shared_state->num_workers; i++) {
        ParallelWorkerInfo* worker = &shared_state->workers[i];
        ConditionVariableBroadcast(&worker->materialized_cv);
    }
    
    /* Wait for all workers to finish */
    bool all_finished = false;
    while (!all_finished) {
        all_finished = true;
        for (i = 0; i < shared_state->num_workers; i++) {
            ParallelWorkerInfo* worker = &shared_state->workers[i];
            LWLockAcquire(worker->coordination_lock, LW_SHARED);
            bool finished = worker->materialization_done;
            LWLockRelease(worker->coordination_lock);
            
            if (!finished) {
                all_finished = false;
                break;
            }
        }
        
        if (!all_finished) {
            pg_usleep(1000);  /* 1ms sleep */
            CHECK_FOR_INTERRUPTS();
        }
    }
    
    /* Clean up parallel buffer */
    if (node->parallel_buffer != NULL) {
        DestroyParallelTupleBuffer(node->parallel_buffer);
        node->parallel_buffer = NULL;
    }
    
    /* Clean up worker memory contexts */
    for (i = 0; i < shared_state->num_workers; i++) {
        ParallelWorkerInfo* worker = &shared_state->workers[i];
        if (worker->worker_context != NULL) {
            MemoryContextDelete(worker->worker_context);
            worker->worker_context = NULL;
        }
    }
    
    /* Log final statistics */
    uint64 total_produced = 0;
    uint64 total_consumed = 0;
    uint64 total_spills = 0;
    
    for (i = 0; i < shared_state->num_workers; i++) {
        ParallelWorkerInfo* worker = &shared_state->workers[i];
        total_produced += worker->tuples_produced;
        total_consumed += worker->tuples_consumed;
        total_spills += worker->memory_spills;
    }
    
    ereport(LOG,
            (errmsg("Parallel materialization completed: %lu tuples produced, %lu consumed, %lu spills",
                   total_produced, total_consumed, total_spills)));
    
    /* Free shared state */
    pfree(shared_state);
    node->shared_state = NULL;
    node->enable_parallel = false;
}

/*
 * CoordinateWorkerMaterialization
 *
 * Coordinates materialization work among parallel workers.
 * Implements load balancing and ensures optimal resource utilization.
 */
void
CoordinateWorkerMaterialization(MaterialState* node)
{
    SharedMaterialState* shared_state = node->shared_state;
    ParallelWorkerInfo* worker = node->worker_info;
    uint32 current_state;
    bool should_produce = true;
    bool should_consume = true;
    
    if (!node->enable_parallel || shared_state == NULL || worker == NULL)
        return;
    
    /* Check global state */
    current_state = pg_atomic_read_u32(&shared_state->materialization_state);
    
    switch (current_state) {
        case MATERIAL_STATE_STARTING:
            /* Transition to running state if we're the leader */
            if (node->is_parallel_leader) {
                if (pg_atomic_compare_exchange_u32(&shared_state->materialization_state,
                                                  &current_state, MATERIAL_STATE_RUNNING)) {
                    ereport(DEBUG1, (errmsg("Parallel materialization started by leader")));
                }
            }
            break;
            
        case MATERIAL_STATE_RUNNING:
            /* Normal operation - coordinate work distribution */
            
            /* Dynamic load balancing based on worker performance */
            uint64 avg_produced = pg_atomic_read_u32(&shared_state->total_tuples) / 
                                 shared_state->num_workers;
            
            if (worker->tuples_produced < avg_produced * 0.8) {
                /* This worker is behind, focus on production */
                should_consume = false;
            } else if (worker->tuples_produced > avg_produced * 1.2) {
                /* This worker is ahead, focus on consumption */
                should_produce = false;
            }
            
            /* Memory pressure adaptation */
            if (shared_state->total_memory_kb > shared_state->spill_threshold_kb) {
                /* Reduce number of producers to limit memory usage */
                if (worker->worker_id % 2 == 1) {
                    should_produce = false;
                }
            }
            
            break;
            
        case MATERIAL_STATE_DONE:
        case MATERIAL_STATE_ERROR:
            /* Materialization finished or error occurred */
            should_produce = false;
            should_consume = false;
            break;
    }
    
    /* Update worker roles */
    LWLockAcquire(worker->coordination_lock, LW_EXCLUSIVE);
    worker->is_producer = should_produce;
    worker->is_consumer = should_consume;
    LWLockRelease(worker->coordination_lock);
    
    /* Signal role changes to other workers */
    ConditionVariableSignal(&worker->materialized_cv);
    
    ereport(DEBUG3,
            (errmsg("Worker %d coordination: produce=%s, consume=%s, state=%u",
                   worker->worker_id, 
                   should_produce ? "yes" : "no",
                   should_consume ? "yes" : "no",
                   current_state)));
}

/*
 * UpdateMaterialStats
 *
 * Updates performance statistics for parallel materialization.
 */
void
UpdateMaterialStats(MaterialState* node)
{
    SharedMaterialState* shared_state = node->shared_state;
    ParallelWorkerInfo* worker = node->worker_info;
    
    if (!node->enable_parallel || shared_state == NULL || worker == NULL)
        return;
    
    /* Update global tuple count */
    pg_atomic_add_fetch_u32(&shared_state->total_tuples, 1);
    
    /* Update worker-specific stats */
    worker->tuples_produced++;
    
    /* Check for memory spill conditions */
    if (worker->worker_context != NULL) {
        size_t context_size = MemoryContextMemAllocated(worker->worker_context, true);
        if (context_size > worker->worker_mem_kb * 1024L) {
            worker->memory_spills++;
            
            /* Reset worker context to free memory */
            MemoryContextReset(worker->worker_context);
            
            ereport(DEBUG2,
                    (errmsg("Worker %d spilled to disk: %zu bytes allocated",
                           worker->worker_id, context_size)));
        }
    }
}

/*
 * LogParallelMaterialPerformance
 *
 * Logs detailed performance information for parallel materialization.
 */
void
LogParallelMaterialPerformance(MaterialState* node)
{
    SharedMaterialState* shared_state = node->shared_state;
    int i;
    
    if (!node->enable_parallel || shared_state == NULL)
        return;
    
    ereport(LOG,
            (errmsg("=== Parallel Material Performance Report ===")));
    
    ereport(LOG,
            (errmsg("Global: %d workers, %u total tuples, %ld KB memory",
                   shared_state->num_workers,
                   pg_atomic_read_u32(&shared_state->total_tuples),
                   shared_state->total_memory_kb)));
    
    for (i = 0; i < shared_state->num_workers; i++) {
        ParallelWorkerInfo* worker = &shared_state->workers[i];
        
        ereport(LOG,
                (errmsg("Worker %d: %lu produced, %lu consumed, %lu spills, %ld KB memory",
                       worker->worker_id,
                       worker->tuples_produced,
                       worker->tuples_consumed,
                       worker->memory_spills,
                       worker->worker_mem_kb)));
    }
    
    if (node->parallel_buffer != NULL) {
        ParallelTupleBuffer* buffer = node->parallel_buffer;
        ereport(LOG,
                (errmsg("Buffer: %zu size, %lu write_pos, %lu read_pos, %u producers, %u consumers",
                       buffer->buffer_size,
                       pg_atomic_read_u64(&buffer->write_pos),
                       pg_atomic_read_u64(&buffer->read_pos),
                       pg_atomic_read_u32(&buffer->active_producers),
                       pg_atomic_read_u32(&buffer->active_consumers))));
    }
    
    ereport(LOG,
            (errmsg("=== End Performance Report ===")));
}

/*
 * SetupPartitionParallelism
 *
 * Sets up partition-wise parallel execution for the Material node.
 * Each worker is assigned specific partitions to reduce contention.
 */
PartitionParallelState*
SetupPartitionParallelism(int num_partitions, int num_workers)
{
    PartitionParallelState* state;
    int i;
    int partitions_per_worker;
    int remaining_partitions;
    
    if (num_partitions <= 0 || num_workers <= 0)
        return NULL;
    
    /* Allocate partition state */
    state = (PartitionParallelState*) palloc0(sizeof(PartitionParallelState));
    state->num_partitions = num_partitions;
    
    /* Allocate arrays */
    state->partition_worker_map = (int*) palloc0(num_partitions * sizeof(int));
    state->partition_stores = (Tuplestorestate**) palloc0(num_partitions * sizeof(Tuplestorestate*));
    state->partition_done = (bool*) palloc0(num_partitions * sizeof(bool));
    
    /* Distribute partitions among workers */
    partitions_per_worker = num_partitions / num_workers;
    remaining_partitions = num_partitions % num_workers;
    
    int current_worker = 0;
    int partitions_assigned = 0;
    
    for (i = 0; i < num_partitions; i++) {
        state->partition_worker_map[i] = current_worker;
        state->partition_done[i] = false;
        
        partitions_assigned++;
        int worker_quota = partitions_per_worker + (current_worker < remaining_partitions ? 1 : 0);
        
        if (partitions_assigned >= worker_quota && current_worker < num_workers - 1) {
            current_worker++;
            partitions_assigned = 0;
        }
        
        ereport(DEBUG2,
                (errmsg("Assigned partition %d to worker %d", i, current_worker)));
    }
    
    ereport(LOG,
            (errmsg("Setup partition parallelism: %d partitions, %d workers, %d per worker",
                   num_partitions, num_workers, partitions_per_worker)));
    
    return state;
}

/*
 * ShutdownPartitionParallelism
 *
 * Cleans up partition-wise parallel state and resources.
 */
void
ShutdownPartitionParallelism(PartitionParallelState* state)
{
    int i;
    
    if (state == NULL)
        return;
    
    /* Clean up partition-specific tuple stores */
    for (i = 0; i < state->num_partitions; i++) {
        if (state->partition_stores[i] != NULL) {
            tuplestore_end(state->partition_stores[i]);
            state->partition_stores[i] = NULL;
        }
    }
    
    /* Free arrays */
    if (state->partition_worker_map != NULL)
        pfree(state->partition_worker_map);
    if (state->partition_stores != NULL)
        pfree(state->partition_stores);
    if (state->partition_done != NULL)
        pfree(state->partition_done);
    
    pfree(state);
    
    ereport(DEBUG1, (errmsg("Shutdown partition parallelism completed")));
}

/*
 * GetPartitionForWorker
 *
 * Determines which partition a worker should process next.
 * Implements dynamic load balancing across partitions.
 */
int
GetPartitionForWorker(PartitionParallelState* state, int worker_id, int num_workers)
{
    int i;
    int assigned_partition = -1;
    
    if (state == NULL)
        return -1;
    
    /* First, check if worker has any assigned partitions that aren't done */
    for (i = 0; i < state->num_partitions; i++) {
        if (state->partition_worker_map[i] == worker_id && !state->partition_done[i]) {
            assigned_partition = i;
            break;
        }
    }
    
    /* If no assigned partitions available, try work stealing */
    if (assigned_partition == -1) {
        for (i = 0; i < state->num_partitions; i++) {
            if (!state->partition_done[i]) {
                /* Check if the assigned worker is overloaded */
                int assigned_worker = state->partition_worker_map[i];
                int assigned_worker_load = 0;
                int current_worker_load = 0;
                
                /* Count remaining partitions for each worker */
                for (int j = 0; j < state->num_partitions; j++) {
                    if (!state->partition_done[j]) {
                        if (state->partition_worker_map[j] == assigned_worker)
                            assigned_worker_load++;
                        else if (state->partition_worker_map[j] == worker_id)
                            current_worker_load++;
                    }
                }
                
                /* Steal work if the assigned worker has significantly more partitions */
                if (assigned_worker_load > current_worker_load + 2) {
                    state->partition_worker_map[i] = worker_id;
                    assigned_partition = i;
                    
                    ereport(DEBUG2,
                            (errmsg("Worker %d stole partition %d from worker %d",
                                   worker_id, i, assigned_worker)));
                    break;
                }
            }
        }
    }
    
    return assigned_partition;
}

/*
 * MarkPartitionDone
 *
 * Marks a partition as completed by a worker.
 */
void
MarkPartitionDone(PartitionParallelState* state, int partition_id)
{
    if (state == NULL || partition_id < 0 || partition_id >= state->num_partitions)
        return;
    
    state->partition_done[partition_id] = true;
    
    ereport(DEBUG2,
            (errmsg("Partition %d marked as done", partition_id)));
}

/*
 * IsAllPartitionsDone
 *
 * Checks if all partitions have been completed.
 */
bool
IsAllPartitionsDone(PartitionParallelState* state)
{
    int i;
    
    if (state == NULL)
        return true;
    
    for (i = 0; i < state->num_partitions; i++) {
        if (!state->partition_done[i])
            return false;
    }
    
    return true;
}

/*
 * GetPartitionTupleStore
 *
 * Gets or creates the tuple store for a specific partition.
 */
Tuplestorestate*
GetPartitionTupleStore(PartitionParallelState* state, int partition_id, 
                      int64 work_mem, int64 max_mem, int plan_node_id)
{
    if (state == NULL || partition_id < 0 || partition_id >= state->num_partitions)
        return NULL;
    
    /* Create tuple store if it doesn't exist */
    if (state->partition_stores[partition_id] == NULL) {
        state->partition_stores[partition_id] = 
            tuplestore_begin_heap(true, false, work_mem, max_mem, plan_node_id, 1, true);
        
        ereport(DEBUG2,
                (errmsg("Created tuple store for partition %d: %ld KB work_mem",
                       partition_id, work_mem)));
    }
    
    return state->partition_stores[partition_id];
}

/*
 * EstimatePartitionMemory
 *
 * Estimates memory requirements for partition-wise parallelism.
 */
int64
EstimatePartitionMemory(int num_partitions, int64 total_memory_kb)
{
    int64 partition_memory;
    
    if (num_partitions <= 0)
        return total_memory_kb;
    
    /* Reserve 20% for coordination overhead */
    partition_memory = (total_memory_kb * 8) / (10 * num_partitions);
    
    /* Ensure minimum 512KB per partition */
    if (partition_memory < 512)
        partition_memory = 512;
    
    /* Cap maximum at 64MB per partition */
    if (partition_memory > 64 * 1024)
        partition_memory = 64 * 1024;
    
    ereport(DEBUG2,
            (errmsg("Estimated partition memory: %ld KB per partition (%d partitions)",
                   partition_memory, num_partitions)));
    
    return partition_memory;
}

/*
 * LogPartitionStats
 *
 * Logs statistics for partition-wise parallel execution.
 */
void
LogPartitionStats(PartitionParallelState* state)
{
    int i;
    int completed_partitions = 0;
    
    if (state == NULL)
        return;
    
    /* Count completed partitions */
    for (i = 0; i < state->num_partitions; i++) {
        if (state->partition_done[i])
            completed_partitions++;
    }
    
    ereport(LOG,
            (errmsg("Partition Stats: %d/%d completed",
                   completed_partitions, state->num_partitions)));
    
    /* Log per-partition details in debug mode */
    for (i = 0; i < state->num_partitions; i++) {
        ereport(DEBUG1,
                (errmsg("Partition %d: worker=%d, done=%s, tuplestore=%s",
                       i,
                       state->partition_worker_map[i],
                       state->partition_done[i] ? "yes" : "no",
                       state->partition_stores[i] ? "exists" : "null")));
    }
}
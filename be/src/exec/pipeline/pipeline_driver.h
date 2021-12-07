// This file is licensed under the Elastic License 2.0. Copyright 2021 StarRocks Limited.

#pragma once

#include <gutil/bits.h>

#include <atomic>

#include "common/statusor.h"
#include "exec/pipeline/fragment_context.h"
#include "exec/pipeline/morsel.h"
#include "exec/pipeline/operator.h"
#include "exec/pipeline/operator_with_dependency.h"
#include "exec/pipeline/pipeline_fwd.h"
#include "exec/pipeline/query_context.h"
#include "exec/pipeline/runtime_filter_types.h"
#include "exec/pipeline/source_operator.h"
#include "util/phmap/phmap.h"
namespace starrocks {
namespace pipeline {

class PipelineDriver;
using DriverPtr = std::shared_ptr<PipelineDriver>;
using Drivers = std::vector<DriverPtr>;

enum DriverState : uint32_t {
    NOT_READY = 0,
    READY = 1,
    RUNNING = 2,
    INPUT_EMPTY = 3,
    OUTPUT_FULL = 4,
    PRECONDITION_BLOCK = 5,
    FINISH = 6,
    CANCELED = 7,
    INTERNAL_ERROR = 8,
    // PENDING_FINISH means a driver's SinkOperator has finished, but its SourceOperator still have a pending
    // io task executed by io threads synchronously, a driver turns to FINISH from PENDING_FINISH after the
    // pending io task's completion.
    PENDING_FINISH = 9,
};

static inline std::string ds_to_string(DriverState ds) {
    switch (ds) {
    case NOT_READY:
        return "NOT_READY";
    case READY:
        return "READY";
    case RUNNING:
        return "RUNNING";
    case INPUT_EMPTY:
        return "INPUT_EMPTY";
    case OUTPUT_FULL:
        return "OUTPUT_FULL";
    case PRECONDITION_BLOCK:
        return "PRECONDITION_BLOCK";
    case FINISH:
        return "FINISH";
    case CANCELED:
        return "CANCELED";
    case INTERNAL_ERROR:
        return "INTERNAL_ERROR";
    case PENDING_FINISH:
        return "PENDING_FINISH";
    }
    DCHECK(false);
    return "UNKNOWN_STATE";
}

// DriverAcct is used to keep statistics of drivers' runtime information, such as time spent
// on core, number of chunks already processed, which are taken into consideration by DriverQueue
// for schedule.
class DriverAcct {
public:
    DriverAcct()

    {}
    //TODO:
    // get_level return a non-negative value that is a hint used by DriverQueue to choose
    // the target internal queue for put_back.
    int get_level() { return Bits::Log2Floor64(schedule_times + 1); }

    // get last elapsed time for process.
    int64_t get_last_time_spent() { return last_time_spent; }

    void update_last_time_spent(int64_t time_spent) {
        this->last_time_spent = time_spent;
        this->accumulated_time_spent += time_spent;
    }
    void update_last_chunks_moved(int64_t chunks_moved) {
        this->last_chunks_moved = chunks_moved;
        this->accumulated_chunk_moved += chunks_moved;
        this->schedule_effective_times += (chunks_moved > 0) ? 1 : 0;
    }
    void update_accumulated_rows_moved(int64_t rows_moved) { this->accumulated_rows_moved += rows_moved; }
    void increment_schedule_times() { this->schedule_times += 1; }

    int64_t get_schedule_times() { return schedule_times; }

    int64_t get_schedule_effective_times() { return schedule_effective_times; }

    int64_t get_rows_per_chunk() {
        if (accumulated_chunk_moved > 0) {
            return accumulated_rows_moved / accumulated_chunk_moved;
        } else {
            return 0;
        }
    }

private:
    int64_t schedule_times{0};
    int64_t schedule_effective_times{0};
    int64_t last_time_spent{0};
    int64_t last_chunks_moved{0};
    int64_t accumulated_time_spent{0};
    int64_t accumulated_chunk_moved{0};
    int64_t accumulated_rows_moved{0};
};

// OperatorExecState is used to guarantee that some hooks of operator
// is called exactly one time during whole operator
enum OperatorStage {
    INIT = 0,
    PREPARED = 1,
    PRECONDITION_NOT_READY = 2,
    PROCESSING = 3,
    FINISHING = 4,
    FINISHED = 5,
    CLOSED = 6,
};

class PipelineDriver {
    friend class PipelineDriverPoller;

public:
    PipelineDriver(const Operators& operators, QueryContext* query_ctx, FragmentContext* fragment_ctx,
                   int32_t driver_id, bool is_root)
            : _operators(operators),
              _first_unfinished(0),
              _query_ctx(query_ctx),
              _fragment_ctx(fragment_ctx),
              _source_node_id(operators[0]->get_plan_node_id()),
              _driver_id(driver_id),
              _is_root(is_root),
              _state(DriverState::NOT_READY),
              _yield_max_chunks_moved(config::pipeline_yield_max_chunks_moved),
              _yield_max_time_spent(config::pipeline_yield_max_time_spent) {
        _runtime_profile = std::make_shared<RuntimeProfile>(strings::Substitute("PipelineDriver (id=$0)", _driver_id));
        for (auto& op : _operators) {
            _operator_stages[op->get_id()] = OperatorStage::INIT;
        }
    }

    PipelineDriver(const PipelineDriver& driver)
            : PipelineDriver(driver._operators, driver._query_ctx, driver._fragment_ctx, driver._driver_id,
                             driver._is_root) {}

    QueryContext* query_ctx() { return _query_ctx; }
    FragmentContext* fragment_ctx() { return _fragment_ctx; }
    int32_t source_node_id() { return _source_node_id; }
    int32_t driver_id() const { return _driver_id; }
    DriverPtr clone() { return std::make_shared<PipelineDriver>(*this); }
    void set_morsel_queue(MorselQueue* morsel_queue) { _morsel_queue = morsel_queue; }
    Status prepare(RuntimeState* runtime_state);
    StatusOr<DriverState> process(RuntimeState* runtime_state);
    void finalize(RuntimeState* runtime_state, DriverState state);
    DriverAcct& driver_acct() { return _driver_acct; }
    DriverState driver_state() const { return _state; }
    void set_driver_state(DriverState state) { _state = state; }
    Operators& operators() { return _operators; }
    SourceOperator* source_operator() { return down_cast<SourceOperator*>(_operators.front().get()); }
    RuntimeProfile* runtime_profile() { return _runtime_profile.get(); }

    // drivers that waits for runtime filters' readiness must be marked PRECONDITION_NOT_READY and put into
    // PipelineDriverPoller.
    void mark_precondition_not_ready();
    // drivers in PRECONDITION_BLOCK state must be marked READY after its dependent runtime-filters or hash tables
    // are finished.
    void mark_precondition_ready(RuntimeState* runtime_state);
    void dispatch_operators();
    // Notify all the unfinished operators to be finished.
    // It is usually used when the sink operator is finished, or the fragment is cancelled or expired.
    void finish_operators(RuntimeState* runtime_state);

    Operator* sink_operator() { return _operators.back().get(); }
    bool is_finished() {
        return _state == DriverState::FINISH || _state == DriverState::CANCELED ||
               _state == DriverState::INTERNAL_ERROR;
    }
    bool pending_finish() { return _state == DriverState::PENDING_FINISH; }
    bool is_still_pending_finish() { return source_operator()->pending_finish() || sink_operator()->pending_finish(); }
    // return false if all the dependencies are ready, otherwise return true.
    bool dependencies_block() {
        if (_all_dependencies_ready) {
            return false;
        }
        _all_dependencies_ready =
                std::all_of(_dependencies.begin(), _dependencies.end(), [](auto& dep) { return dep->is_ready(); });
        return !_all_dependencies_ready;
    }

    // return false if all the local runtime filters are ready, otherwise return false.
    bool local_rf_block() {
        if (_all_local_rf_ready) {
            return false;
        }
        _all_local_rf_ready = std::all_of(_local_rf_holders.begin(), _local_rf_holders.end(),
                                          [](auto* holder) { return holder->is_ready(); });
        return !_all_local_rf_ready;
    }

    bool global_rf_block() {
        if (_all_global_rf_ready_or_timeout) {
            return false;
        }

        _all_global_rf_ready_or_timeout =
                _precondition_block_timer_sw->elapsed_time() >= _global_rf_wait_timeout_ns || // Timeout,
                std::all_of(_global_rf_descriptors.begin(), _global_rf_descriptors.end(),
                            [](auto* rf_desc) { return rf_desc->runtime_filter() != nullptr; }); // or ready.

        return !_all_global_rf_ready_or_timeout;
    }

    // return true if either dependencies_block or local_rf_block return true, which means that the current driver
    // should wait for both hash table and local runtime filters' readiness.
    bool is_precondition_block() { return dependencies_block() || local_rf_block() || global_rf_block(); }

    bool is_not_blocked() {
        if (_state == DriverState::PRECONDITION_BLOCK) {
            return !is_precondition_block();
        } else if (_state == DriverState::OUTPUT_FULL) {
            return sink_operator()->need_input() || sink_operator()->is_finished();
        } else if (_state == DriverState::INPUT_EMPTY) {
            return source_operator()->has_output() || source_operator()->is_finished();
        }
        return true;
    }

    bool is_root() const { return _is_root; }

    std::string to_readable_string() const;

private:
    // check whether fragment is cancelled. It is used before pull_chunk and push_chunk.
    bool _check_fragment_is_canceled(RuntimeState* runtime_state);
    void _mark_operator_finishing(OperatorPtr& op, RuntimeState* runtime_state);
    void _mark_operator_finished(OperatorPtr& op, RuntimeState* runtime_state);
    void _mark_operator_closed(OperatorPtr& op, RuntimeState* runtime_state);
    void _close_operators(RuntimeState* runtime_state);

    Operators _operators;

    DriverDependencies _dependencies;
    bool _all_dependencies_ready = false;

    mutable std::vector<RuntimeFilterHolder*> _local_rf_holders;
    bool _all_local_rf_ready = false;

    std::vector<vectorized::RuntimeFilterProbeDescriptor*> _global_rf_descriptors;
    bool _all_global_rf_ready_or_timeout = false;
    int64_t _global_rf_wait_timeout_ns = -1;

    size_t _first_unfinished;
    QueryContext* _query_ctx;
    FragmentContext* _fragment_ctx;
    // The default value -1 means no source
    int32_t _source_node_id = -1;
    int32_t _driver_id;
    const bool _is_root;
    DriverAcct _driver_acct;
    // The first one is source operator
    MorselQueue* _morsel_queue = nullptr;
    DriverState _state;
    std::shared_ptr<RuntimeProfile> _runtime_profile = nullptr;
    const size_t _yield_max_chunks_moved;
    const int64_t _yield_max_time_spent;

    phmap::flat_hash_map<int32_t, OperatorStage> _operator_stages;

    // metrics
    RuntimeProfile::Counter* _total_timer = nullptr;
    RuntimeProfile::Counter* _active_timer = nullptr;
    RuntimeProfile::Counter* _pending_timer = nullptr;
    RuntimeProfile::Counter* _precondition_block_timer = nullptr;
    RuntimeProfile::Counter* _local_rf_waiting_set_counter = nullptr;

    RuntimeProfile::Counter* _schedule_counter = nullptr;
    RuntimeProfile::Counter* _schedule_effective_counter = nullptr;
    RuntimeProfile::Counter* _schedule_rows_per_chunk = nullptr;

    MonotonicStopWatch* _total_timer_sw = nullptr;
    MonotonicStopWatch* _pending_timer_sw = nullptr;
    MonotonicStopWatch* _precondition_block_timer_sw = nullptr;
};

} // namespace pipeline
} // namespace starrocks

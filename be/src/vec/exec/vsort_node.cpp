// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include "vec/exec/vsort_node.h"

#include "exec/sort_exec_exprs.h"
#include "runtime/row_batch.h"
#include "runtime/runtime_state.h"
#include "util/debug_util.h"
#include "vec/core/sort_block.h"
#include "vec/utils/util.hpp"

namespace doris::vectorized {

VSortNode::VSortNode(ObjectPool* pool, const TPlanNode& tnode, const DescriptorTbl& descs)
        : ExecNode(pool, tnode, descs),
          _offset(tnode.sort_node.__isset.offset ? tnode.sort_node.offset : 0),
          _num_rows_skipped(0),
          _unsorted_block(nullptr) {}

Status VSortNode::init(const TPlanNode& tnode, RuntimeState* state) {
    RETURN_IF_ERROR(ExecNode::init(tnode, state));
    RETURN_IF_ERROR(_vsort_exec_exprs.init(tnode.sort_node.sort_info, _pool));
    _is_asc_order = tnode.sort_node.sort_info.is_asc_order;
    _nulls_first = tnode.sort_node.sort_info.nulls_first;
    return Status::OK();
}

Status VSortNode::prepare(RuntimeState* state) {
    SCOPED_TIMER(_runtime_profile->total_time_counter());
    _runtime_profile->add_info_string("TOP-N", _limit == -1 ? "false" : "true");
    RETURN_IF_ERROR(ExecNode::prepare(state));
    SCOPED_CONSUME_MEM_TRACKER(_mem_tracker.get());
    RETURN_IF_ERROR(_vsort_exec_exprs.prepare(state, child(0)->row_desc(), _row_descriptor));
    _unsorted_block.reset(new MutableBlock(
            VectorizedUtils::create_empty_columnswithtypename(child(0)->row_desc())));
    _partial_sort_timer = ADD_TIMER(_runtime_profile, "PartialSortTime");
    return Status::OK();
}

Status VSortNode::open(RuntimeState* state) {
    START_AND_SCOPE_SPAN(state->get_tracer(), span, "VSortNode::open");
    SCOPED_TIMER(_runtime_profile->total_time_counter());
    RETURN_IF_ERROR(ExecNode::open(state));
    SCOPED_CONSUME_MEM_TRACKER(_mem_tracker.get());
    RETURN_IF_ERROR(_vsort_exec_exprs.open(state));
    RETURN_IF_CANCELLED(state);
    RETURN_IF_ERROR(state->check_query_state("vsort, while open."));
    RETURN_IF_ERROR(child(0)->open(state));

    // The child has been opened and the sorter created. Sort the input.
    // The final merge is done on-demand as rows are requested in get_next().
    RETURN_IF_ERROR(sort_input(state));

    // Unless we are inside a subplan expecting to call open()/get_next() on the child
    // again, the child can be closed at this point.
    // if (!IsInSubplan()) {
    //    child(0)->close(state);
    // }
    return Status::OK();
}

Status VSortNode::get_next(RuntimeState* state, RowBatch* row_batch, bool* eos) {
    *eos = true;
    return Status::NotSupported("Not Implemented VSortNode::get_next scalar");
}

Status VSortNode::get_next(RuntimeState* state, Block* block, bool* eos) {
    INIT_AND_SCOPE_GET_NEXT_SPAN(state->get_tracer(), _get_next_span, "VSortNode::get_next");
    SCOPED_TIMER(_runtime_profile->total_time_counter());
    SCOPED_CONSUME_MEM_TRACKER(_mem_tracker.get());

    auto status = Status::OK();
    if (_sorted_blocks.empty()) {
        *eos = true;
    } else if (_sorted_blocks.size() == 1) {
        if (_offset != 0) {
            _sorted_blocks[0].skip_num_rows(_offset);
        }
        block->swap(_sorted_blocks[0]);
        *eos = true;
    } else {
        RETURN_IF_ERROR(merge_sort_read(state, block, eos));
    }

    reached_limit(block, eos);
    return status;
}

Status VSortNode::reset(RuntimeState* state) {
    _num_rows_skipped = 0;
    return Status::OK();
}

Status VSortNode::close(RuntimeState* state) {
    if (is_closed()) {
        return Status::OK();
    }
    START_AND_SCOPE_SPAN(state->get_tracer(), span, "VSortNode::close");
    _vsort_exec_exprs.close(state);
    return ExecNode::close(state);
}

void VSortNode::debug_string(int indentation_level, stringstream* out) const {
    *out << string(indentation_level * 2, ' ');
    *out << "VSortNode(";
    for (int i = 0; i < _is_asc_order.size(); ++i) {
        *out << (i > 0 ? " " : "") << (_is_asc_order[i] ? "asc" : "desc") << " nulls "
             << (_nulls_first[i] ? "first" : "last");
    }
    ExecNode::debug_string(indentation_level, out);
    *out << ")";
}

Status VSortNode::sort_input(RuntimeState* state) {
    bool eos = false;
    do {
        do {
            Block upstream_block;
            RETURN_IF_ERROR_AND_CHECK_SPAN(
                    child(0)->get_next_after_projects(state, &upstream_block, &eos),
                    child(0)->get_next_span(), eos);
            if (upstream_block.rows() != 0) {
                _unsorted_block->merge(upstream_block);
            }
        } while (!eos && _unsorted_block->rows() < BUFFERED_BLOCK_SIZE &&
                 _unsorted_block->allocated_bytes() < BUFFERED_BLOCK_BYTES);
        if (_unsorted_block->rows() > 0) {
            _total_mem_usage += _unsorted_block->allocated_bytes();
            Block block = _unsorted_block->to_block(0);
            {
                SCOPED_TIMER(_partial_sort_timer);
                RETURN_IF_ERROR(partial_sort(block));
            }
            // dispose TOP-N logic
            if (_limit != -1) {
                // Here is a little opt to reduce the mem uasge, we build a max heap
                // to order the block in _block_priority_queue.
                // if one block totally greater the heap top of _block_priority_queue
                // we can throw the block data directly.
                if (_num_rows_in_block < _limit) {
                    _sorted_blocks.emplace_back(std::move(block));
                    _num_rows_in_block += block.rows();
                    _block_priority_queue.emplace(_pool->add(
                            new SortCursorImpl(_sorted_blocks.back(), _sort_description)));
                } else {
                    SortBlockCursor block_cursor(
                            _pool->add(new SortCursorImpl(block, _sort_description)));
                    if (!block_cursor.totally_greater(_block_priority_queue.top())) {
                        _sorted_blocks.emplace_back(std::move(block));
                        _block_priority_queue.push(block_cursor);
                    } else {
                        continue;
                    }
                }
            } else {
                // dispose normal sort logic
                _sorted_blocks.emplace_back(std::move(block));
            }
            RETURN_IF_CANCELLED(state);
            RETURN_IF_ERROR(state->check_query_state("vsort, while sorting input."));
            _unsorted_block.reset(new MutableBlock(
                    VectorizedUtils::create_empty_columnswithtypename(child(0)->row_desc())));
        }
    } while (!eos);

    build_merge_tree();
    return Status::OK();
}

Status VSortNode::partial_sort(doris::vectorized::Block& block) {
    if (_vsort_exec_exprs.need_materialize_tuple()) {
        auto output_tuple_expr_ctxs = _vsort_exec_exprs.sort_tuple_slot_expr_ctxs();
        std::vector<int> valid_column_ids(output_tuple_expr_ctxs.size());
        for (int i = 0; i < output_tuple_expr_ctxs.size(); ++i) {
            RETURN_IF_ERROR(output_tuple_expr_ctxs[i]->execute(&block, &valid_column_ids[i]));
        }

        Block new_block;
        for (auto column_id : valid_column_ids) {
            new_block.insert(block.get_by_position(column_id));
        }
        block.swap(new_block);
    }

    _sort_description.resize(_vsort_exec_exprs.lhs_ordering_expr_ctxs().size());
    for (int i = 0; i < _sort_description.size(); i++) {
        const auto& ordering_expr = _vsort_exec_exprs.lhs_ordering_expr_ctxs()[i];
        RETURN_IF_ERROR(ordering_expr->execute(&block, &_sort_description[i].column_number));

        _sort_description[i].direction = _is_asc_order[i] ? 1 : -1;
        _sort_description[i].nulls_direction =
                _nulls_first[i] ? -_sort_description[i].direction : _sort_description[i].direction;
    }

    sort_block(block, _sort_description, _offset + _limit);

    return Status::OK();
}

void VSortNode::build_merge_tree() {
    for (const auto& block : _sorted_blocks) {
        _cursors.emplace_back(block, _sort_description);
    }

    if (_sorted_blocks.size() > 1) {
        for (auto& _cursor : _cursors) _priority_queue.push(SortCursor(&_cursor));
    }
}

Status VSortNode::merge_sort_read(doris::RuntimeState* state, doris::vectorized::Block* block,
                                  bool* eos) {
    size_t num_columns = _sorted_blocks[0].columns();

    bool mem_reuse = block->mem_reuse();
    MutableColumns merged_columns =
            mem_reuse ? block->mutate_columns() : _sorted_blocks[0].clone_empty_columns();

    /// Take rows from queue in right order and push to 'merged'.
    size_t merged_rows = 0;
    while (!_priority_queue.empty()) {
        auto current = _priority_queue.top();
        _priority_queue.pop();

        if (_offset == 0) {
            for (size_t i = 0; i < num_columns; ++i)
                merged_columns[i]->insert_from(*current->all_columns[i], current->pos);
            ++merged_rows;
        } else {
            _offset--;
        }

        if (!current->isLast()) {
            current->next();
            _priority_queue.push(current);
        }

        if (merged_rows == state->batch_size()) break;
    }

    if (merged_rows == 0) {
        *eos = true;
        return Status::OK();
    }

    if (!mem_reuse) {
        Block merge_block = _sorted_blocks[0].clone_with_columns(std::move(merged_columns));
        merge_block.swap(*block);
    }

    return Status::OK();
}

} // namespace doris::vectorized

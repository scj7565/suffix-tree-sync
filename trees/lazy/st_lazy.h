// Copyright 2025 Snowflake Inc.
// SPDX-License-Identifier: Apache-2.0
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#pragma once

#include <atomic>
#include <cassert>
#include <deque>
#include <memory>
#include <span>
#include <utility>
#include <vector>

#include "../../third_party/arctic/int32_map.h"
#include "../../third_party/arctic/suffix_tree.h"   // ::Draft (측정 틀과 같은 타입)
#include "../../common/locks.h"                      // [lazy] 삭제 대기 목록 보호용 Mutex

namespace lazy {
using ::Draft;

struct Group;

struct Node {
    // Number of suffixes from the root that end at or pass through this node.
    int64_t count = 0;

    // Token referenced by this node. Node can refer to a sequence of tokens,
    // this is just the ID of the first token.
    int token = 0;

    // Number of tokens in this node.
    int length = 0;

    // Reference sequence ID and starting index for the tokens in this node.
    // Implements the path compression optimization to achieve O(N) memory.
    int ref_seq = 0;
    int ref_idx = -1;

    // This map tracks all the suffixes that end at this node. Maps seq_id to
    // the end index of that suffix (may be truncated due to tree depth). Used
    // to find a new ref_seq and ref_idx if the reference sequence is deleted.
    Int32Map<int> endpoints;

    // Pointer to parent node.
    Node* parent = nullptr;

    // Children nodes, the key should always be the first token of the child.
    Int32Map<std::unique_ptr<Node>> children;

    // All the children of each node are kept in order of decreasing count in
    // a doubly linked list for efficient speculation. head_child points to the
    // first child (highest count) and tail_child points to the last child.
    Node* head_child = nullptr;
    Node* tail_child = nullptr;

    // Pointers to the next and previous siblings in the doubly linked list.
    Node* next_sibling = nullptr;
    Node* prev_sibling = nullptr;

    // To enable efficient reordering of the siblings list when counts change,
    // nodes with the same count are grouped together. Each node holds a shared
    // pointer to its group, and the groups also form a doubly linked list.
    std::shared_ptr<Group> group = nullptr;

    Node() = default;

    Node(int64_t count, int token, int length, int ref_seq, int ref_idx)
        : count(count),
          token(token),
          length(length),
          ref_seq(ref_seq),
          ref_idx(ref_idx) {}

    // Memory usage of this node.
    size_t memory_usage() const {
        size_t total = sizeof(*this);
        total += children.memory_usage();
        total += endpoints.memory_usage();
        return total;
    }
};

struct Group {
    // Pointer to the head node of this group. All nodes before the head node
    // have a strictly higher count, and all nodes after the head node have a
    // lower or equal count.
    Node* head = nullptr;

    // Pointers to the next and previous groups in the doubly linked list.
    Group* next = nullptr;
    Group* prev = nullptr;

    Group(Node* head) : head(head) {}
};


class SuffixTree {
public:

    SuffixTree(int max_depth);

    int num_seqs() const {
        return static_cast<int>(_seqs.size());
    }

    // Append a new element to the sequence with id seq_id.
    void append(int seq_id, int token);

    // Append multiple new elements to the sequence with id seq_id.
    void extend(int seq_id, std::span<const int32_t> tokens);

    // Remove the sequence with id seq_id.
    void remove(int seq_id);

    // Given a context, speculate the next tokens using the suffix tree.
    Draft speculate(std::span<const int32_t> context,
                    int max_spec_tokens,
                    float max_spec_factor,
                    float max_spec_offset,
                    float min_token_prob,
                    bool use_tree_spec);

    // Check the integrity of the suffix tree, return empty string if ok,
    // otherwise return an error message.
    std::string check_integrity();

    // Estimate memory usage of the suffix tree, for debugging only. It
    // walks the entire tree so can be slow.
    size_t estimate_memory() const;

    // [suffix-tree-sync 추가] 트리 "내용"의 요약값 (--check 용, 한 스레드에서만 호출).
    // 루트에서 각 노드까지의 토큰열과 count 만 보고, 형제 순서·ref_seq 같은 표현 차이는 무시한다.
    // → 갱신 순서가 달라도 같은 토큰들을 넣었으면 같은 값이 나와야 한다.
    uint64_t content_digest();

    // [lazy] [한 스레드] 병렬 구간 전에 seq 자리를 만들고 reserve (병렬 구간에서 맵 rehash·재할당 방지).
    void prepare_seq(int seq_id, int max_len);

    // [lazy] 삭제는 세 단계로 미룬다.
    //  1) mark_removed : 큐에 넣기만 (extend 와 동시에 불러도 됨)
    //  2) _apply_marks : count 줄이기. count 0 노드(좀비)는 추측에서 건너뜀 (갱신 구간 뒤 첫 speculate)
    //  3) flush_removed: 좀비 떼기·합치기·ref 갱신 (end_step)
    // 전제: 대기 중인 id 를 flush 전에 다시 extend 하지 않는다 (다시 쓰려면 prepare_seq 먼저).
    void mark_removed(int seq_id);

    // [lazy] 3단계 (end_step, 한 스레드). 지운 seq 수를 돌려준다.
    int flush_removed();

private:

    // [lazy] 삭제 대기: _queued(1단계) → _marked(2단계 끝). _pending_set 은 둘을 합친 집합.
    std::vector<int32_t> _queued;
    std::vector<int32_t> _marked;
    Int32Map<char> _pending_set;
    Mutex _pending_mu;
    std::atomic<bool> _marks_pending{false};   // _queued 가 비어 있지 않음 (speculate 가 먼저 확인)
    Mutex _apply_mu;                           // 2단계를 한 스레드만 하도록

    void _apply_marks();                       // 2단계, 1스레드 (end_step, prepare_seq)
    void _apply_marks_if_pending();            // speculate 앞에서: 추측 스레드들이 나눠서 2단계
    void _build_batch();                       // 큐 → 서브트리별 할 일 목록 (한 스레드)
    void _run_batch_group(int g, bool lock_root);   // 서브트리 g 의 접미사들 count 줄이기
    bool _batch_built = false;                 // _apply_mu 로 보호
    std::vector<std::pair<int32_t, int32_t>> _batch[64];   // 서브트리별 (seq_id, 시작 위치), serial 순서
    std::atomic<int> _batch_next{0};           // 다음에 가져갈 서브트리 번호
    std::atomic<int> _batch_done{0};           // 끝난 서브트리 수

    // [lazy] 락 순서는 루트 -> 서브트리. 서브트리 락은 첫 토큰 서브트리, 루트 락은 루트와 깊이 1 형제 리스트.
    static constexpr int kSubtreeLocks = 64;
    struct alignas(64) PaddedLock { Mutex m; };   // 락마다 캐시 줄 하나 (거짓 공유 방지)
    alignas(64) Mutex _root_lock;
    PaddedLock _subtree_locks[kSubtreeLocks];

    // [lazy] node 의 count 0 자식(형제 리스트 맨 뒤)을 서브트리째 떼어 낸다. node 의 락을 잡고 부른다.
    int _purge_zombies(Node* node);

    // [lazy] 물리적 삭제 본체 (seq 하나, 한 스레드)
    void _physical_remove(int seq_id);

    // Maximum depth of the suffix tree.
    int _max_depth;

    // The root node of the suffix tree.
    std::unique_ptr<Node> _root;

    // Mapping from seq id to its sequence of tokens (vectors of int32_t).
    Int32Map<std::vector<int32_t>> _seqs;

    // For each sequence, a sliding window of active nodes. Maintains at most
    // _max_depth active nodes for each sequence. Queue is shifted when a new
    // token is added to the sequence. Each active node is in the queue for at
    // most _max_depth iterations before being removed.
    Int32Map<std::deque<Node*>> _active_nodes;

    std::pair<Node*, int> _match_context(std::span<const int32_t> context);

    Draft _speculate_path(Node* node, int idx, int max_spec_tokens,
                          float min_token_prob);

    Draft _speculate_tree(Node* node, int idx, int max_spec_tokens,
                          float min_token_prob);

    std::string _check_node_integrity(Node* node);
};

}  // namespace lazy

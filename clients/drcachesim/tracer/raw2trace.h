/* **********************************************************
 * Copyright (c) 2016-2025 Google, Inc.  All rights reserved.
 * **********************************************************/

/*
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * * Redistributions of source code must retain the above copyright notice,
 *   this list of conditions and the following disclaimer.
 *
 * * Redistributions in binary form must reproduce the above copyright notice,
 *   this list of conditions and the following disclaimer in the documentation
 *   and/or other materials provided with the distribution.
 *
 * * Neither the name of Google, Inc. nor the names of its contributors may be
 *   used to endorse or promote products derived from this software without
 *   specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL VMWARE, INC. OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH
 * DAMAGE.
 */

/* raw2trace.h: shared defines between the tracer and the converter.
 */

#ifndef _RAW2TRACE_H_
#define _RAW2TRACE_H_ 1

/**
 * @file drmemtrace/raw2trace.h
 * @brief DrMemtrace offline trace post-processing customization.
 */

#define NOMINMAX // Avoid windows.h messing up std::min.
#include <stddef.h>
#include <stdint.h>

#include <array>
#include <atomic>
#include <bitset>
#include <deque>
#include <fstream>
#include <limits>
#include <list>
#include <map>
#include <memory>
#include <queue>
#include <set>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include "archive_ostream.h"
#include "dr_api.h"
#include "drmemtrace.h"
#include "hashtable.h"
#include "instru.h"
#include "raw2trace_shared.h"
#include "reader.h"
#include "record_file_reader.h"
#include "schedule_file.h"
#include "trace_entry.h"
#include "utils.h"
#ifdef BUILD_PT_POST_PROCESSOR
#    include "pt2ir.h"
#endif

namespace dynamorio {
namespace drmemtrace {

#ifdef DEBUG
#    define DEBUG_ASSERT(x) DR_ASSERT(x)
#else
#    define DEBUG_ASSERT(x) /* nothing */
#endif

#ifdef HAS_LZ4
#    define TRACE_SUFFIX_LZ4 "trace.lz4"
#endif

#ifdef HAS_ZIP
#    define TRACE_SUFFIX_ZIP "trace.zip"
#endif

#ifdef HAS_ZLIB
#    define TRACE_SUFFIX_GZ "trace.gz"
#endif

#define TRACE_SUFFIX "trace"

typedef enum {
    RAW2TRACE_STAT_COUNT_ELIDED,
    RAW2TRACE_STAT_DUPLICATE_SYSCALL,
    RAW2TRACE_STAT_RSEQ_ABORT,
    RAW2TRACE_STAT_RSEQ_SIDE_EXIT,
    RAW2TRACE_STAT_FALSE_SYSCALL,
    RAW2TRACE_STAT_EARLIEST_TRACE_TIMESTAMP,
    RAW2TRACE_STAT_LATEST_TRACE_TIMESTAMP,
    RAW2TRACE_STAT_FINAL_TRACE_INSTRUCTION_COUNT,
    RAW2TRACE_STAT_KERNEL_INSTR_COUNT,
    RAW2TRACE_STAT_SYSCALL_TRACES_CONVERTED,
    // Count of PT syscall traces that could not be converted and were skipped
    // in the final trace.
    RAW2TRACE_STAT_SYSCALL_TRACES_CONVERSION_FAILED,
    // Count of decoding errors that were not fatal to the conversion of the
    // RAW2TRACE_STAT_SYSCALL_TRACES_CONVERTED traces. These result in some
    // 1-instr PC discontinuities in the syscall trace (<= 1 per non-fatal
    // error).
    RAW2TRACE_STAT_SYSCALL_TRACES_NON_FATAL_DECODING_ERROR_COUNT,
    // Count of PT syscall traces that turned up empty. This may have been
    // simply because the syscall was interrupted and therefore no PT data
    // was recorded.
    RAW2TRACE_STAT_SYSCALL_TRACES_CONVERSION_EMPTY,
    RAW2TRACE_STAT_SYSCALL_TRACES_INJECTED,
    // We add a MAX member so that we can iterate over all stats in unit tests.
    RAW2TRACE_STAT_MAX,
} raw2trace_statistic_t;

/**
 * instr_summary_t is a compact encapsulation of the information needed by trace
 * conversion from decoded instructions.
 */
struct instr_summary_t final {
    /**
     * Caches information about a single memory reference.
     * Note that we reuse the same memref_summary_t object in raw2trace for all
     * memrefs of a scatter/gather instr. To avoid any issues due to mismatch
     * between instru_offline (which sees the expanded scatter/gather instr seq)
     * and raw2trace (which sees the original app scatter/gather instr only), we
     * disable address elision for scatter/gather bbs.
     */
    struct memref_summary_t {
        memref_summary_t(opnd_t opnd)
            : opnd(opnd)
            , remember_base(0)
            , use_remembered_base(0)
        {
        }
        /** The addressing mode of this reference. */
        opnd_t opnd;
        /**
         * A flag for reconstructing elided same-base addresses.  If set, this
         * address should be remembered for use on a later reference with the same
         * base and use_remembered_base set.
         */
        bool remember_base : 1;
        /**
         * A flag for reconstructing elided same-base addresses.  If set, this
         * address is not present in the trace and should be filled in from the prior
         * reference with the same base and remember_base set, or from the PC for
         * a rip-relative reference.
         */
        bool use_remembered_base : 1;
    };

    instr_summary_t()
    {
    }

    /**
     * Populates a pre-allocated instr_summary_t description, from the instruction found
     * at pc. Updates pc to the next instruction. Optionally logs translation details
     * (using orig_pc and verbosity).
     */
    static bool
    construct(void *dcontext, app_pc block_pc, DR_PARAM_INOUT app_pc *pc, app_pc orig_pc,
              DR_PARAM_OUT instr_summary_t *desc, uint verbosity = 0);

    /**
     * Get the pc after the instruction that was used to produce this instr_summary_t.
     */
    app_pc
    next_pc() const
    {
        return pc_ + length_;
    }
    /** Get the pc of the start of this instrucion. */
    app_pc
    pc() const
    {
        return pc_;
    }

    /**
     * Sets properties of the "pos"-th source memory operand by OR-ing in the
     * two boolean values.
     */
    void
    set_mem_src_flags(size_t pos, bool use_remembered_base, bool remember_base)
    {
        DEBUG_ASSERT(pos < mem_srcs_and_dests_.size());
        auto target = &mem_srcs_and_dests_[pos];
        target->use_remembered_base = target->use_remembered_base || use_remembered_base;
        target->remember_base = target->remember_base || remember_base;
    }

    /**
     * Sets properties of the "pos"-th destination memory operand by OR-ing in the
     * two boolean values.
     */
    void
    set_mem_dest_flags(size_t pos, bool use_remembered_base, bool remember_base)
    {
        DEBUG_ASSERT(num_mem_srcs_ + pos < mem_srcs_and_dests_.size());
        auto target = &mem_srcs_and_dests_[num_mem_srcs_ + pos];
        target->use_remembered_base = target->use_remembered_base || use_remembered_base;
        target->remember_base = target->remember_base || remember_base;
    }

private:
    friend class raw2trace_t;

    byte
    length() const
    {
        return length_;
    }
    uint16_t
    type() const
    {
        return type_;
    }
    uint16_t
    prefetch_type() const
    {
        return prefetch_type_;
    }
    uint16_t
    flush_type() const
    {
        return flush_type_;
    }

    bool
    reads_memory() const
    {
        return TESTANY(kReadsMemMask, packed_);
    }
    bool
    writes_memory() const
    {
        return TESTANY(kWritesMemMask, packed_);
    }
    bool
    is_prefetch() const
    {
        return TESTANY(kIsPrefetchMask, packed_);
    }
    bool
    is_flush() const
    {
        return TESTANY(kIsFlushMask, packed_);
    }
#ifdef AARCH64
    bool
    is_aarch64_dc_zva() const
    {
        return TESTANY(kIsAarch64DcZvaMask, packed_);
    }
#endif
    bool
    is_cti() const
    {
        return TESTANY(kIsCtiMask, packed_);
    }
    bool
    is_scatter_or_gather() const
    {
        return TESTANY(kIsScatterOrGatherMask, packed_);
    }
    bool
    is_syscall() const
    {
        return TESTANY(kIsSyscallMask, packed_);
    }

    const memref_summary_t &
    mem_src_at(size_t pos) const
    {
        return mem_srcs_and_dests_[pos];
    }
    const memref_summary_t &
    mem_dest_at(size_t pos) const
    {
        return mem_srcs_and_dests_[num_mem_srcs_ + pos];
    }
    size_t
    num_mem_srcs() const
    {
        return num_mem_srcs_;
    }
    size_t
    num_mem_dests() const
    {
        return mem_srcs_and_dests_.size() - num_mem_srcs_;
    }
    // Returns 0 for indirect branches or non-branches.
    app_pc
    branch_target_pc() const
    {
        return branch_target_pc_;
    }

    static const int kReadsMemMask = 0x0001;
    static const int kWritesMemMask = 0x0002;
    static const int kIsPrefetchMask = 0x0004;
    static const int kIsFlushMask = 0x0008;
    static const int kIsCtiMask = 0x0010;

    // kIsAarch64DcZvaMask is available during processing of non-AArch64 traces too, but
    // it's intended for use only for AArch64 traces. This declaration reserves the
    // assigned mask and makes it unavailable for future masks.
    static const int kIsAarch64DcZvaMask = 0x0020;

    static const int kIsScatterOrGatherMask = 0x0040;

    static const int kIsSyscallMask = 0x0080;

    instr_summary_t(const instr_summary_t &other) = delete;
    instr_summary_t &
    operator=(const instr_summary_t &) = delete;
    instr_summary_t(instr_summary_t &&other) = delete;
    instr_summary_t &
    operator=(instr_summary_t &&) = delete;

    app_pc pc_ = 0;
    uint16_t type_ = 0;
    uint16_t prefetch_type_ = 0;
    uint16_t flush_type_ = 0;
    byte length_ = 0;
    app_pc branch_target_pc_ = 0;

    // Squash srcs and dests to save memory usage. We may want to
    // bulk-allocate pages of instr_summary_t objects, instead
    // of piece-meal allocating them on the heap one at a time.
    // One vector and a byte is smaller than 2 vectors.
    std::vector<memref_summary_t> mem_srcs_and_dests_;
    uint8_t num_mem_srcs_ = 0;
    byte packed_ = 0;
};

/**
 * Functions for encoding memtrace data headers. Each function returns the number of bytes
 * the write operation required: sizeof(trace_entry_t). The buffer is assumed to be
 * sufficiently large.
 */
struct trace_metadata_writer_t {
    static int
    write_thread_exit(byte *buffer, thread_id_t tid);
    static int
    write_marker(byte *buffer, trace_marker_type_t type, uintptr_t val);
    static int
    write_iflush(byte *buffer, addr_t start, size_t size);
    static int
    write_pid(byte *buffer, process_id_t pid);
    static int
    write_tid(byte *buffer, thread_id_t tid);
    static int
    write_timestamp(byte *buffer, uint64 timestamp);
};

/**
 * Header of raw trace.
 */
struct trace_header_t {
    process_id_t pid;
    thread_id_t tid;
    uint64 timestamp;
    size_t cache_line_size;
};

/**
 * Bitset hash table for balancing search time in case of enormous count of pc.
 * Each pc represented as pair of high 64-BLOCK_SIZE_BIT bits and
 * lower BLOCK_SIZE_BIT bits.
 * High bits is the key in hash table and give bitset table with BLOCK_SIZE size.
 * Lower bits set bit in bitset that means this pc was processed.
 * BLOCK_SIZE_BIT=13 was picked up to exclude hash collision and save speed up.
 */

template <typename T> class bitset_hash_table_t {
private:
    const static size_t BLOCK_SIZE_BIT = 13;
    const static size_t BLOCK_SIZE = (1 << BLOCK_SIZE_BIT);
    const size_t BASIC_BUCKET_COUNT = (1 << 15);
    std::unordered_map<T, std::bitset<BLOCK_SIZE>> page_table_;
    typename std::unordered_map<T, std::bitset<BLOCK_SIZE>>::iterator last_block_ =
        page_table_.end();

    inline std::pair<T, ushort>
    convert(T pc)
    {
        return std::pair<T, ushort>(
            reinterpret_cast<T>(reinterpret_cast<size_t>(pc) & (~(BLOCK_SIZE - 1))),
            static_cast<ushort>(reinterpret_cast<size_t>(pc) & (BLOCK_SIZE - 1)));
    }

public:
    bitset_hash_table_t()
    {
        static_assert(std::is_pointer<T>::value || std::is_integral<T>::value,
                      "Pointer or integral type required");
        page_table_.reserve(BASIC_BUCKET_COUNT);
        page_table_.emplace(T(0), std::move(std::bitset<BLOCK_SIZE>()));
        last_block_ = page_table_.begin();
    }

    bool
    find_and_insert(T pc)
    {
        auto block = convert(pc);
        if (block.first != last_block_->first) {
            last_block_ = page_table_.find(block.first);
            if (last_block_ == page_table_.end()) {
                last_block_ = (page_table_.emplace(std::make_pair(
                                   block.first, std::move(std::bitset<BLOCK_SIZE>()))))
                                  .first;
                last_block_->second[block.second] = 1;
                return true;
            }
        }
        if (last_block_->second[block.second] == 1)
            return false;
        last_block_->second[block.second] = 1;
        return true;
    }

    void
    erase(T pc)
    {
        auto block = convert(pc);
        if (last_block_->first != block.first)
            last_block_ = page_table_.find(block.first);
        if (last_block_ != page_table_.end())
            last_block_->second[block.second] = 0;
    }

    void
    clear()
    {
        page_table_.clear();
        page_table_.reserve(BASIC_BUCKET_COUNT);
        page_table_.emplace(T(0), std::move(std::bitset<BLOCK_SIZE>()));
        last_block_ = page_table_.begin();
    }

    ~bitset_hash_table_t()
    {
        clear();
    }
};

/**
 * The raw2trace class converts the raw offline trace format to the format
 * expected by analysis tools.  It requires access to the binary files for the
 * libraries and executable that were present during tracing.
 */
class raw2trace_t {
public:
    /* TODO i#6145: The argument list of raw2trace_t has become excessively long. It would
     * be more manageable to have an options struct instead.
     */
    // Only one of out_files and out_archives should be non-empty: archives support fast
    // seeking and are preferred but require zlib.
    // module_map, encoding_file, serial_schedule_file, cpu_schedule_file, thread_files,
    // and out_files are all owned and opened/closed by the caller.  module_map is not a
    // string and can contain binary data.
    // If a nullptr dcontext is passed, creates a new DR context va dr_standalone_init().
    raw2trace_t(
        const char *module_map, const std::vector<std::istream *> &thread_files,
        const std::vector<std::ostream *> &out_files,
        const std::vector<archive_ostream_t *> &out_archives,
        file_t encoding_file = INVALID_FILE, std::ostream *serial_schedule_file = nullptr,
        archive_ostream_t *cpu_schedule_file = nullptr, void *dcontext = nullptr,
        unsigned int verbosity = 0, int worker_count = -1,
        const std::string &alt_module_dir = "",
        uint64_t chunk_instr_count = 10 * 1000 * 1000,
        const std::unordered_map<thread_id_t, std::istream *> &kthread_files_map = {},
        const std::string &kcore_path = "", const std::string &kallsyms_path = "",
        std::unique_ptr<dynamorio::drmemtrace::record_reader_t> syscall_template_file =
            nullptr,
        bool pt2ir_best_effort = false);
    // If a nullptr dcontext_in was passed to the constructor, calls dr_standalone_exit().
    virtual ~raw2trace_t();

    /**
     * Adds handling for custom data fields that were stored with each module via
     * drmemtrace_custom_module_data() during trace generation.  When do_conversion()
     * or do_module_parsing() is subsequently called, its parsing of the module data
     * will invoke \p parse_cb, which should advance the module data pointer passed
     * in \p src and return it as its return value (or nullptr on error), returning
     * the resulting parsed data in \p data.  The \p data pointer will later be
     * passed to both \p process_cb, which can update the module path inside \p info
     * (and return a non-empty string on error), and \b free_cb, which can perform
     * cleanup.
     *
     * A custom callback value \p process_cb_user_data can be passed to \p
     * process_cb.  The same is not provided for the other callbacks as they end up
     * using the drmodtrack_add_custom_data() framework where there is no support for
     * custom callback parameters.
     *
     * Returns a non-empty error message on failure.
     */
    std::string
    handle_custom_data(const char *(*parse_cb)(const char *src, DR_PARAM_OUT void **data),
                       std::string (*process_cb)(drmodtrack_info_t *info, void *data,
                                                 void *user_data),
                       void *process_cb_user_data, void (*free_cb)(void *data));

    /**
     * Performs the first step of do_conversion() without further action: parses and
     * iterates over the list of modules.  This is provided to give the user a method
     * for iterating modules in the presence of the custom field used by drmemtrace
     * that prevents direct use of drmodtrack_offline_read().
     * On success, calls the \p process_cb function passed to handle_custom_data()
     * for every module in the list, and returns an empty string at the end.
     * Returns a non-empty error message on failure.
     *
     * \deprecated #dynamorio::drmemtrace::module_mapper_t should be used instead.
     */
    std::string
    do_module_parsing();

    /**
     * This interface is meant to be used with a final trace rather than a raw
     * trace, using the module log file saved from the raw2trace conversion.
     * This routine first calls do_module_parsing() and then maps each module into
     * the current address space, allowing the user to augment the instruction
     * information in the trace with additional information by decoding the
     * instruction bytes.  The routine find_mapped_trace_address() should be used
     * to convert from memref_t.instr.addr to the corresponding mapped address in
     * the current process.
     * Returns a non-empty error message on failure.
     *
     * \deprecated #dynamorio::drmemtrace::module_mapper_t::get_loaded_modules() should be
     * used instead.
     */
    std::string
    do_module_parsing_and_mapping();

    /**
     * This interface is meant to be used with a final trace rather than a raw
     * trace, using the module log file saved from the raw2trace conversion.
     * When do_module_parsing_and_mapping() has been called, this routine can be used
     * to convert an instruction program counter in a trace into an address in the
     * current process where the instruction bytes for that instruction are mapped,
     * allowing decoding for obtaining further information than is stored in the trace.
     * Returns a non-empty error message on failure.
     *
     * \deprecated #dynamorio::drmemtrace::module_mapper_t::find_mapped_trace_address()
     * should be used instead.
     */
    std::string
    find_mapped_trace_address(app_pc trace_address, DR_PARAM_OUT app_pc *mapped_address);

    /**
     * Performs the conversion from raw data to finished trace files.
     * Returns a non-empty error message on failure.
     */
    virtual std::string
    do_conversion();

    static std::string
    check_thread_file(std::istream *f);

    /**
     * Writes the essential header entries to the given buffer. This is useful for other
     * libraries that want to create a trace that works with our tools like the analyzer
     * framework.
     */
    static void
    create_essential_header_entries(byte *&buf_ptr, int version,
                                    offline_file_type_t file_type, thread_id_t tid,
                                    process_id_t pid);

#ifdef BUILD_PT_POST_PROCESSOR
    /**
     *  Checks whether the given file is a valid kernel PT file.
     */
    static std::string
    check_kthread_file(std::istream *f);

    /**
     *  Return the tid of the given kernel PT file.
     */
    static std::string
    get_kthread_file_tid(std::istream *f, DR_PARAM_OUT thread_id_t *tid);
#endif

    uint64
    get_statistic(raw2trace_statistic_t stat);

protected:
    /**
     * The trace_entry_t buffer returned by get_write_buffer() is assumed to be at least
     * #WRITE_BUFFER_SIZE large.
     *
     * WRITE_BUFFER_SIZE needs to be large enough to hold one instruction and its
     * memrefs.
     * Some of the AArch64 SVE scatter/gather instructions have a lot of memref entries.
     * For example ld4b loads 4 registers with byte sized elements, so that is
     *     (vl_bits / 8) * 4
     * entries. With a 512-bit vector length that is (512 / 8) * 4 = 256 memref entries.
     */
    static const uint WRITE_BUFFER_SIZE = 260;

    struct block_summary_t {
        block_summary_t(app_pc start, int instr_count)
            : start_pc(start)
            , instrs(instr_count)
        {
        }
        app_pc start_pc;
        std::vector<instr_summary_t> instrs;
    };

    struct branch_info_t {
        branch_info_t(app_pc pc, app_pc target, int idx)
            : pc(pc)
            , target_pc(target)
            , buf_idx(idx)
        {
        }
        branch_info_t()
            : pc(0)
            , target_pc(0)
            , buf_idx(-1)
        {
        }
        app_pc pc;
        app_pc target_pc;
        int buf_idx; // Index into rseq_buffer_.
    };

    // Per-traced-thread data is stored here and accessed without locks by having each
    // traced thread processed by only one processing thread.
    struct raw2trace_thread_data_t {
        raw2trace_thread_data_t()
            : index(0)
            , tid(0)
            , worker(0)
            , thread_file(nullptr)
            , out_archive(nullptr)
            , out_file(nullptr)
            , version(0)
            , file_type(OFFLINE_FILE_TYPE_DEFAULT)
            , saw_header(false)
            , last_entry_is_split(false)
            , prev_instr_was_rep_string(false)
            , last_decode_block_start(nullptr)
            , last_decode_modidx(0)
            , last_decode_modoffs(0)
            , last_block_summary(nullptr)
#ifdef BUILD_PT_POST_PROCESSOR
            , kthread_file(nullptr)
#endif
        {
        }
        // Support subclasses extending this struct.
        virtual ~raw2trace_thread_data_t()
        {
        }

        int index;
        thread_id_t tid;
        int worker;
        std::istream *thread_file;
        archive_ostream_t *out_archive; // May be nullptr.
        std::ostream *out_file;         // Always set; for archive, == "out_archive".
        std::string error;
        int version;
        offline_file_type_t file_type;
        size_t cache_line_size = 0;
        std::deque<offline_entry_t> pre_read;

        // Used to delay a thread-buffer-final branch to keep it next to its target.
        std::vector<trace_entry_t> delayed_branch;
        // This is the first step of optimization of using delayed_branch_ vector.
        // Checking the bool value is cheaper than delayed_branch.empty().
        // In general it's better to avoid this vector at all.
        bool delayed_branch_empty_ = true;
        // Records the decode pcs for delayed_branch instructions for re-inserting
        // encodings across a chunk boundary.
        std::vector<app_pc> delayed_branch_decode_pcs;
        // Records the targets for delayed branches.  We don't merge this with
        // delayed_branch and delayed_branch_decode_pcs as the other vectors are
        // passed as raw arrays to write().
        std::vector<app_pc> delayed_branch_target_pcs;

        // Current trace conversion state.
        bool saw_header;
        offline_entry_t last_entry;
        // For 2-entry markers we need a 2nd current-entry struct we can unread.
        bool last_entry_is_split;
        offline_entry_t last_split_first_entry;
        std::array<trace_entry_t, WRITE_BUFFER_SIZE> out_buf;
        bool prev_instr_was_rep_string;
        // There is no sentinel available for modidx+modoffs so we use the pc for that.
        app_pc last_decode_block_start;
        uint64 last_decode_modidx;
        uint64 last_decode_modoffs;
        block_summary_t *last_block_summary;
        uint64 last_window = 0;

        // Statistics on the processing.
        uint64 count_elided = 0;
        uint64 count_duplicate_syscall = 0;
        uint64 count_false_syscall = 0;
        uint64 count_rseq_abort = 0;
        uint64 count_rseq_side_exit = 0;
        uint64 earliest_trace_timestamp = (std::numeric_limits<uint64>::max)();
        uint64 latest_trace_timestamp = 0;
        uint64 final_trace_instr_count = 0;
        uint64 kernel_instr_count = 0;
        uint64 syscall_traces_converted = 0;
        uint64 syscall_traces_conversion_failed = 0;
        uint64 syscall_traces_non_fatal_decoding_error_count = 0;
        uint64 syscall_traces_conversion_empty = 0;
        uint64 syscall_traces_injected = 0;

        uint64 cur_chunk_instr_count = 0;
        uint64 cur_chunk_ref_count = 0;
        memref_counter_t memref_counter;
        uint64 chunk_count_ = 0;
        uint64 last_timestamp_ = 0;
        uint last_cpu_ = 0;
        app_pc last_pc_fallthrough_if_syscall_ = 0;

        bitset_hash_table_t<app_pc> encoding_emitted;
        app_pc last_encoding_emitted = nullptr;

        schedule_file_t::per_shard_t sched_data;

        // State for rolling back rseq aborts and side exits.
        bool rseq_want_rollback_ = false;
        bool rseq_ever_saw_entry_ = false;
        bool rseq_buffering_enabled_ = false;
        bool rseq_past_end_ = false;
        addr_t rseq_commit_pc_ = 0;
        addr_t rseq_start_pc_ = 0;
        addr_t rseq_end_pc_ = 0;
        int to_inject_syscall_ = INJECT_NONE;
        bool saw_first_func_id_marker_after_syscall_ = false;
        std::vector<trace_entry_t> rseq_buffer_;
        int rseq_commit_idx_ = -1; // Index into rseq_buffer_.
        std::vector<branch_info_t> rseq_branch_targets_;
        std::vector<app_pc> rseq_decode_pcs_;

#ifdef BUILD_PT_POST_PROCESSOR
        std::unique_ptr<drir_t> pt_decode_state_ = nullptr;
        std::istream *kthread_file;
        bool pt_metadata_processed = false;
        pt2ir_t pt2ir;
#endif

        // Sentinel value for to_inject_syscall.
        static constexpr int INJECT_NONE = -1;
    };

#ifdef BUILD_PT_POST_PROCESSOR
    /**
     * Returns the next #pt_data_buf_t entry from the thread's kernel raw file. If the
     * next entry is also the first one, the thread's pt_metadata is also returned in the
     * provided parameter.
     */
    virtual std::unique_ptr<pt_data_buf_t>
    get_next_kernel_entry(raw2trace_thread_data_t *tdata,
                          std::unique_ptr<pt_metadata_buf_t> &pt_metadata,
                          uint64_t syscall_idx);

#endif

    /**
     * Convert starting from in_entry, and reading more entries as required.
     * Sets end_of_record to true if processing hit the end of a record.
     * read_and_map_modules() must have been called by the implementation before
     * calling this API.
     */
    bool
    process_offline_entry(raw2trace_thread_data_t *tdata, const offline_entry_t *in_entry,
                          thread_id_t tid, DR_PARAM_OUT bool *end_of_record,
                          DR_PARAM_OUT bool *last_bb_handled,
                          DR_PARAM_OUT bool *flush_decode_cache);

    /**
     * Called for each record in an output buffer prior to writing out the buffer.
     * The entry cannot be modified.  A subclass can override this to compute
     * per-shard statistics which can then be used for a variety of tasks including
     * late removal of shards for targeted filtering.
     */
    virtual void
    observe_entry_output(raw2trace_thread_data_t *tls, const trace_entry_t *entry);

    /**
     * Performs processing actions for the marker "marker_type" with value
     * "marker_val", including writing out a marker record.  Further records can also
     * be written to "buf".  Returns whether successful.
     */
    virtual bool
    process_marker(raw2trace_thread_data_t *tdata, trace_marker_type_t marker_type,
                   uintptr_t marker_val, byte *&buf,
                   DR_PARAM_OUT bool *flush_decode_cache);
    /**
     * Read the header of a thread, by calling get_next_entry() successively to
     * populate the header values. The timestamp field is populated only
     * for legacy traces.
     */
    bool
    read_header(raw2trace_thread_data_t *tdata, DR_PARAM_OUT trace_header_t *header);

    /**
     * Point to the next offline entry_t.  Will not attempt to dereference past the
     * returned pointer.
     */
    virtual const offline_entry_t *
    get_next_entry(raw2trace_thread_data_t *tdata);

    /**
     * Records the currently stored last entry in order to remember two entries at once
     * (for handling split two-entry markers) and then reads and returns a pointer to the
     * next entry.  A subsequent call to unread_last_entry() will put back both entries.
     * Returns an emptry string on success or an error description on an error.
     */
    virtual const offline_entry_t *
    get_next_entry_keep_prior(raw2trace_thread_data_t *tdata);

    /* Adds the last read entry to the front of the read queue for get_next_entry(). */
    virtual void
    unread_last_entry(raw2trace_thread_data_t *tdata);

    /* Adds "entry" to the back of the read queue for get_next_entry(). */
    void
    queue_entry(raw2trace_thread_data_t *tdata, offline_entry_t &entry);

    /**
     * Callback notifying the currently-processed thread has exited. Subclasses are
     * expected to track record metadata themselves.  APIs for extracting that metadata
     * are exposed.
     */
    virtual bool
    on_thread_end(raw2trace_thread_data_t *tdata);

    /**
     * The level parameter represents severity: the lower the level, the higher the
     * severity.
     */
    virtual void
    log(uint level, const char *fmt, ...);

    /**
     * Similar to log() but this disassembles the given PC.
     */
    virtual void
    log_instruction(uint level, app_pc decode_pc, app_pc orig_pc);

    virtual std::string
    read_and_map_modules();

#ifdef BUILD_PT_POST_PROCESSOR
    /**
     * Process the PT data associated with the provided syscall index.
     */
    bool
    process_syscall_pt(raw2trace_thread_data_t *tdata, uint64_t syscall_idx);
#endif

    /**
     * Processes a raw buffer which must be the next buffer in the desired (typically
     * timestamp-sorted) order for its traced thread.  For concurrent buffer processing,
     * all buffers from any one traced thread must be processed by the same worker
     * thread, both for correct ordering and correct synchronization.
     */
    bool
    process_next_thread_buffer(raw2trace_thread_data_t *tdata,
                               DR_PARAM_OUT bool *end_of_record);

    bool
    maybe_inject_pending_syscall_sequence(raw2trace_thread_data_t *tdata,
                                          const offline_entry_t &entry, byte *buf_base);

    std::string
    aggregate_and_write_schedule_files();

    bool
    write_footer(raw2trace_thread_data_t *tdata);

    bool
    open_new_chunk(raw2trace_thread_data_t *tdata);

    /**
     * Reads entries in the given system call template file. These will be added
     * to the final trace at the locations of the corresponding system call number
     * markers.
     */
    std::string
    read_syscall_template_file();

    /**
     * Returns the app pc of the first instruction in the system call template
     * read for syscall_num. Returns nullptr if it could not find it.
     */
    app_pc
    get_first_app_pc_for_syscall_template(int syscall_num);

    /**
     * Writes the system call template to the output trace, if any was provided in
     * the system call template file for the given syscall_num.
     */
    bool
    write_syscall_template(raw2trace_thread_data_t *tdata, byte *&buf,
                           trace_entry_t *buf_base, int syscall_num);

    /**
     * The pointer to the DR context.
     */
    void *const dcontext_;

    /**
     * Whether a non-nullptr dcontext was passed to the constructor.
     */
    bool passed_dcontext_ = false;

    /* TODO i#2062: Remove this after replacing all uses in favor of queries to
     * module_mapper_t to handle both generated and module code.
     */
    /**
     * Get the module map.
     */
    const std::vector<module_t> &
    modvec_() const
    {
        return *modvec_ptr_;
    }

    /* TODO i#2062: Remove this after replacing all uses in favor of queries to
     * module_mapper_t to handle both generated and module code.
     */
    /**
     * Set the module map. Must be called before process_offline_entry() is called.
     */
    void
    set_modvec_(const std::vector<module_t> *modvec)
    {
        modvec_ptr_ = modvec;
    }

    /**
     * Get the module mapper.
     */
    const module_mapper_t &
    modmap_() const
    {
        return *modmap_ptr_;
    }

    /**
     * Set the module mapper. Must be called before process_offline_entry() is called.
     */
    void
    set_modmap_(const module_mapper_t *modmap)
    {
        modmap_ptr_ = modmap;
    }

    /** Returns whether this system number *might* block. */
    virtual bool
    is_maybe_blocking_syscall(uintptr_t number);

    const module_mapper_t *modmap_ptr_ = nullptr;

    uint64 count_elided_ = 0;
    uint64 count_duplicate_syscall_ = 0;
    uint64 count_false_syscall_ = 0;
    uint64 count_rseq_abort_ = 0;
    uint64 count_rseq_side_exit_ = 0;
    uint64 earliest_trace_timestamp_ = (std::numeric_limits<uint64>::max)();
    uint64 latest_trace_timestamp_ = 0;
    uint64 final_trace_instr_count_ = 0;
    uint64 kernel_instr_count_ = 0;
    uint64 syscall_traces_converted_ = 0;
    uint64 syscall_traces_conversion_failed_ = 0;
    uint64 syscall_traces_non_fatal_decoding_error_count_ = 0;
    uint64 syscall_traces_conversion_empty_ = 0;
    uint64 syscall_traces_injected_ = 0;

    std::unique_ptr<module_mapper_t> module_mapper_;

    std::vector<std::unique_ptr<raw2trace_thread_data_t>> thread_data_;

private:
    // We store this in drmodtrack_info_t.custom to combine our binary contents
    // data with any user-added module data from drmemtrace_custom_module_data.
    struct custom_module_data_t {
        size_t contents_size;
        const char *contents;
        void *user_data;
    };

    // Return a writable buffer guaranteed to be at least #WRITE_BUFFER_SIZE large.
    //  get_write_buffer() may reuse the same buffer after write() or
    //  write_delayed_branches()
    // is called.
    trace_entry_t *
    get_write_buffer(raw2trace_thread_data_t *tdata);

    // Writes the converted traces between start and end, where end is past the last
    // item to write. Both start and end are assumed to be pointers inside a buffer
    // returned by get_write_buffer().  decode_pcs is only needed if there is more
    // than one instruction in the buffer; in that case it must contain one entry per
    // instruction.
    bool
    write(raw2trace_thread_data_t *tdata, const trace_entry_t *start,
          const trace_entry_t *end, app_pc *decode_pcs = nullptr,
          size_t decode_pcs_size = 0);

    // Similar to write(), but treat the provided traces as delayed branches: if they
    // are the last values in a record, they belong to the next record of the same
    // thread.  The start..end sequence must contain one instruction.
    bool
    write_delayed_branches(raw2trace_thread_data_t *tdata, const trace_entry_t *start,
                           const trace_entry_t *end, app_pc decode_pc = nullptr,
                           app_pc target_pc = nullptr);

    // Writes encoding entries for pc..pc+instr_length to buf.
    bool
    append_encoding(raw2trace_thread_data_t *tdata, app_pc pc, size_t instr_length,
                    trace_entry_t *&buf, trace_entry_t *buf_start);

    bool
    insert_post_chunk_encodings(raw2trace_thread_data_t *tdata,
                                const trace_entry_t *instr, app_pc decode_pc);

    // Returns true if there are currently delayed branches that have not been emitted
    // yet.
    bool
    delayed_branches_exist(raw2trace_thread_data_t *tdata);

    // Returns false if an encoding was already emitted.
    // Otherwise, remembers that an encoding is being emitted now, and returns true.
    bool
    record_encoding_emitted(raw2trace_thread_data_t *tdata, app_pc pc);

    // Removes the record of the last encoding remembered in record_encoding_emitted().
    // This can only be called once between calls to record_encoding_emitted()
    // and only after record_encoding_emitted() returns true.
    void
    rollback_last_encoding(raw2trace_thread_data_t *tdata);

    // Writes out the buffered entries for an rseq region, after rolling back to
    // a side exit or abort if necessary.
    bool
    adjust_and_emit_rseq_buffer(raw2trace_thread_data_t *tdata, addr_t next_pc,
                                addr_t abort_pc = 0);

    // Removes entries from tdata->rseq_buffer_ between and including the instructions
    // starting at or after remove_start_rough_idx and before or equal to
    // remove_end_rough_idx.  These "rough" indices can be on the encoding or instr
    // fetch to include that instruction.
    bool
    rollback_rseq_buffer(raw2trace_thread_data_t *tdata, int remove_start_rough_idx,
                         // This is inclusive.
                         int remove_end_rough_idx);

    // Returns whether an #instr_summary_t representation of the instruction at pc inside
    // the block that begins at block_start_pc in the specified module exists.
    bool
    instr_summary_exists(raw2trace_thread_data_t *tdata, uint64 modidx, uint64 modoffs,
                         app_pc block_start, int index, app_pc pc);
    block_summary_t *
    lookup_block_summary(raw2trace_thread_data_t *tdata, uint64 modidx, uint64 modoffs,
                         app_pc block_start);
    instr_summary_t *
    lookup_instr_summary(raw2trace_thread_data_t *tdata, uint64 modidx, uint64 modoffs,
                         app_pc block_start, int index, app_pc pc,
                         DR_PARAM_OUT block_summary_t **block_summary);
    instr_summary_t *
    create_instr_summary(raw2trace_thread_data_t *tdata, uint64 modidx, uint64 modoffs,
                         block_summary_t *block, app_pc block_start, int instr_count,
                         int index, DR_PARAM_INOUT app_pc *pc, app_pc orig);

    // Return the #instr_summary_t representation of the index-th instruction (at *pc)
    // inside the block that begins at block_start_pc and contains instr_count
    // instructions in the specified module.  Updates the value at pc to the PC of the
    // next instruction. It is assumed the app binaries have already been loaded using
    // #module_mapper_t, and the values at *pc point within memory mapped by the module
    // mapper. This API provides an opportunity to cache decoded instructions.
    const instr_summary_t *
    get_instr_summary(raw2trace_thread_data_t *tdata, uint64 modidx, uint64 modoffs,
                      app_pc block_start, int instr_count, int index,
                      DR_PARAM_INOUT app_pc *pc, app_pc orig);

    // Sets two flags stored in the memref_summary_t inside the instr_summary_t for the
    // index-th instruction (at pc) inside the block that begins at block_start_pc and
    // contains instr_count instructions in the specified module.  The flags
    // use_remembered_base and remember_base are set for the source (write==false) or
    // destination (write==true) operand of index memop_index. The flags are OR-ed in:
    // i.e., they are never cleared.
    bool
    set_instr_summary_flags(raw2trace_thread_data_t *tdata, uint64 modidx, uint64 modoffs,
                            app_pc block_start, int instr_count, int index, app_pc pc,
                            app_pc orig, bool write, int memop_index,
                            bool use_remembered_base, bool remember_base);
    void
    set_last_pc_fallthrough_if_syscall(raw2trace_thread_data_t *tdata, app_pc value);
    app_pc
    get_last_pc_fallthrough_if_syscall(raw2trace_thread_data_t *tdata);

    // Sets a per-traced-thread cached flag that is read by was_prev_instr_rep_string().
    void
    set_prev_instr_rep_string(raw2trace_thread_data_t *tdata, bool value);

    // Queries a per-traced-thread cached flag that is set by set_prev_instr_rep_string().
    bool
    was_prev_instr_rep_string(raw2trace_thread_data_t *tdata);

    // Returns the trace file version (an OFFLINE_FILE_VERSION* constant).
    int
    get_version(raw2trace_thread_data_t *tdata);

    // Returns the trace file type (a combination of OFFLINE_FILE_TYPE* constants).
    offline_file_type_t

    get_file_type(raw2trace_thread_data_t *tdata);
    void
    set_file_type(raw2trace_thread_data_t *tdata, offline_file_type_t file_type);

    size_t
    get_cache_line_size(raw2trace_thread_data_t *tdata);

    // Accumulates the given value into the per-thread value for the statistic
    // identified by stat. This may involve a simple addition, or any other operation
    // like std::min or std::max, depending on the statistic.
    void
    accumulate_to_statistic(raw2trace_thread_data_t *tdata, raw2trace_statistic_t stat,
                            uint64 value);
    void
    log_instruction(app_pc decode_pc, app_pc orig_pc);

    // Flush the branches sent to write_delayed_branches().
    bool
    append_delayed_branch(raw2trace_thread_data_t *tdata, app_pc next_pc);

    bool
    thread_file_at_eof(raw2trace_thread_data_t *tdata);
    bool
    process_header(raw2trace_thread_data_t *tdata);

    bool
    process_thread_file(raw2trace_thread_data_t *tdata);

    void
    process_tasks(std::vector<raw2trace_thread_data_t *> *tasks);

    bool
    emit_new_chunk_header(raw2trace_thread_data_t *tdata);

    bool
    analyze_elidable_addresses(raw2trace_thread_data_t *tdata, uint64 modidx,
                               uint64 modoffs, app_pc start_pc, uint instr_count);

    // Returns true if a kernel interrupt happened at cur_pc. The check skips
    // memrefs to search for a kernel event marker. If the kernel event marker has
    // the value of cur_pc, it returns true.
    bool
    interrupted_by_kernel_event(raw2trace_thread_data_t *tdata, uint64_t cur_pc,
                                uint64_t cur_offs);

    bool
    append_bb_entries(raw2trace_thread_data_t *tdata, const offline_entry_t *in_entry,
                      DR_PARAM_OUT bool *handled);

    // Returns true if an rseq abort happened at cur_pc.
    // Outputs a kernel interrupt if this is the right location.
    // Outputs any other markers observed if !instrs_are_separate, since they
    // are part of this block and need to be inserted now. Inserts all
    // intra-block markers (i.e., the higher level process_offline_entry() will
    // never insert a marker intra-block) and all inter-block markers are
    // handled at a higher level (process_offline_entry()) and are never
    // inserted here.
    bool
    handle_rseq_abort_marker(raw2trace_thread_data_t *tdata,
                             DR_PARAM_INOUT trace_entry_t **buf_in, uint64_t cur_pc,
                             uint64_t cur_offs, DR_PARAM_OUT bool *rseq_aborted);

    bool
    get_marker_value(raw2trace_thread_data_t *tdata,
                     DR_PARAM_INOUT const offline_entry_t **entry,
                     DR_PARAM_OUT uintptr_t *value);

    bool
    append_memref(raw2trace_thread_data_t *tdata, DR_PARAM_INOUT trace_entry_t **buf_in,
                  const instr_summary_t *instr, instr_summary_t::memref_summary_t memref,
                  bool write, std::unordered_map<reg_id_t, addr_t> &reg_vals,
                  DR_PARAM_OUT bool *reached_end_of_memrefs);

    bool
    should_omit_syscall(raw2trace_thread_data_t *tdata);

    bool
    is_marker_type(const offline_entry_t *entry, trace_marker_type_t marker_type);

    int worker_count_;
    std::vector<std::vector<raw2trace_thread_data_t *>> worker_tasks_;

    class block_hashtable_t {
        // We use a hashtable to cache decodings.  We compared the performance of
        // hashtable_t to std::map.find, std::map.lower_bound, std::tr1::unordered_map,
        // and c++11 std::unordered_map (including tuning its load factor, initial size,
        // and hash function), and hashtable_t outperformed the others (i#2056).
        // Update: that measurement was when we did a hashtable lookup on every
        // instruction pc.  Now that we use block_summary_t and only look up each block,
        // the hashtable performance matters much less.
        // Plus, for 32-bit we cannot fit our modidx:modoffs key in the 32-bit-limited
        // hashtable_t key, so we now use std::unordered_map there.
    public:
        explicit block_hashtable_t(int worker_count)
        {
#ifdef X64
            // We go ahead and start with a reasonably large capacity.
            // We do not want the built-in mutex: this is per-worker so it can be
            // lockless.
            hashtable_init_ex(&table, 16, HASH_INTPTR, false, false, free_payload,
                              nullptr, nullptr);
            // We pay a little memory to get a lower load factor, unless we have
            // many duplicated tables.
            hashtable_config_t config = { sizeof(config), true,
                                          worker_count <= 8
                                              ? 40U
                                              : (worker_count <= 16 ? 50U : 60U) };
            hashtable_configure(&table, &config);
#endif
        }
#ifdef X64
        ~block_hashtable_t()
        {
            hashtable_delete(&table);
        }
#else
        // Work around a known Visual Studio issue where it complains about deleted copy
        // constructors for unique_ptr by deleting our copies and defaulting our moves.
        block_hashtable_t(const block_hashtable_t &) = delete;
        block_hashtable_t &
        operator=(const block_hashtable_t &) = delete;
        block_hashtable_t(block_hashtable_t &&) = default;
        block_hashtable_t &
        operator=(block_hashtable_t &&) = default;
#endif
        block_summary_t *
        lookup(uint64 modidx, uint64 modoffs)
        {
#ifdef X64
            return static_cast<block_summary_t *>(hashtable_lookup(
                &table, reinterpret_cast<void *>(hash_key(modidx, modoffs))));
#else
            return table[hash_key(modidx, modoffs)].get();
#endif
        }
        // Takes ownership of "block".
        void
        add(uint64 modidx, uint64 modoffs, block_summary_t *block)
        {
#ifdef X64
            hashtable_add(&table, reinterpret_cast<void *>(hash_key(modidx, modoffs)),
                          block);
#else
            table[hash_key(modidx, modoffs)].reset(block);
#endif
        }

        void
        clear()
        {
#ifdef X64
            hashtable_clear(&table);
#else
            table.clear();
#endif
        }

    private:
        static void
        free_payload(void *ptr)
        {
            delete (static_cast<block_summary_t *>(ptr));
        }
        static inline uint64
        hash_key(uint64 modidx, uint64 modoffs)
        {
            return (modidx << PC_MODOFFS_BITS) | modoffs;
        }

#ifdef X64
        hashtable_t table;
#else
        std::unordered_map<uint64, std::unique_ptr<block_summary_t>> table;
#endif
    };

    // We use a per-worker cache to avoid locks.
    std::vector<block_hashtable_t> decode_cache_;

    // Store optional parameters for the module_mapper_t until we need to construct it.
    const char *(*user_parse_)(const char *src, DR_PARAM_OUT void **data) = nullptr;
    void (*user_free_)(void *data) = nullptr;
    std::string (*user_process_)(drmodtrack_info_t *info, void *data,
                                 void *user_data) = nullptr;
    void *user_process_data_ = nullptr;

    const char *modmap_bytes_;
    file_t encoding_file_ = INVALID_FILE;
    std::ostream *serial_schedule_file_ = nullptr;
    archive_ostream_t *cpu_schedule_file_ = nullptr;

    unsigned int verbosity_ = 0;

    std::string alt_module_dir_;

    // Our decode_cache duplication will not scale forever on very large code
    // footprint traces, so we set a cap for the default.
    static const int kDefaultJobMax = 16;

    // Chunking for seeking support in compressed files.
    uint64_t chunk_instr_count_ = 0;

    offline_instru_t instru_offline_;
    const std::vector<module_t> *modvec_ptr_ = nullptr;

    // For decoding kernel PT traces.
    const std::unordered_map<thread_id_t, std::istream *> kthread_files_map_;
    const std::string kcore_path_;
    const std::string kallsyms_path_;

    // For inserting system call traces from provided templates.
    std::unique_ptr<dynamorio::drmemtrace::record_reader_t> syscall_template_file_reader_;

    struct trace_template_t {
        std::vector<trace_entry_t> entries;
        int instr_count = 0;
    };
    std::unordered_map<int, trace_template_t> syscall_trace_templates_;
    memref_counter_t syscall_trace_template_encodings_;
    offline_file_type_t syscall_template_file_type_ = OFFLINE_FILE_TYPE_DEFAULT;

    // Whether conversion of PT raw traces is done on a best-effort basis. This includes
    // ignoring various types of non-fatal decoding errors and still producing a syscall
    // trace where possible (which may have some PC discontinuities), and also dropping
    // some syscall traces completely from the final trace where the PT trace could
    // not be converted.
    bool pt2ir_best_effort_ = false;
};

} // namespace drmemtrace
} // namespace dynamorio

#endif /* _RAW2TRACE_H_ */

/*
 *  Copyright (c) 2000-2022 Inria
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions are met:
 *
 *  * Redistributions of source code must retain the above copyright notice,
 *  this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright notice,
 *  this list of conditions and the following disclaimer in the documentation
 *  and/or other materials provided with the distribution.
 *  * Neither the name of the ALICE Project-Team nor the names of its
 *  contributors may be used to endorse or promote products derived from this
 *  software without specific prior written permission.
 * 
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 *  AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 *  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 *  ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 *  LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 *  CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 *  SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 *  INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 *  CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 *  ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *  POSSIBILITY OF SUCH DAMAGE.
 *
 *  Contact: Bruno Levy
 *
 *     https://www.inria.fr/fr/bruno-levy
 *
 *     Inria,
 *     Domaine de Voluceau,
 *     78150 Le Chesnay - Rocquencourt
 *     FRANCE
 *
 */

#ifndef DELAUNAY_SYNC_H
#define DELAUNAY_SYNC_H

#include <geogram/basic/common.h>
#include <geogram/basic/assert.h>
#include <atomic>

/**
 * \file geogram/delaunay/delaunay_sync.h
 * \brief Synchronization primitives for parallel Delaunay
 */

namespace GEO {

    /**
     * \brief An array of cell status codes associates to each tetrahedron
     *  in a Delaunay tetrahedralization
     * \details Each item can be atomically accessed to implement fine-grained
     *  resource control in a multithreaded context. It is used to memorize
     *  for each tetrahedron the thread that owns it as well as a couple of
     *  flags.
     */
    class CellStatusArray {
    public:
        typedef uint8_t cell_status_t;
        static constexpr cell_status_t FREE_CELL = cell_status_t(-1);

        /**
         * \brief Creates an empty CellStatusArray
         */
        CellStatusArray() : cell_status_(nullptr), size_(0), capacity_(0) {
        }

        /**
         * \brief Creates a CellStatusArray
         * \param[in] size_in number of cells in the CellStatusArray
         */
        CellStatusArray(index_t size_in) :
            cell_status_(nullptr), size_(0), capacity_(0) {
            resize(size_in,size_in);
        }

        /**
         * \brief CellStatusArray destructor
         * \details It is illegal to destroy a CellStatusArray if 
         *  - threads are still running
         *  - there exists a cell with a status different from FREE_CELL
         */
        ~CellStatusArray() {
            clear();
        }

        /**
         * \brief Forbids copy
         */
        CellStatusArray(const CellStatusArray& rhs) = delete;

        /**
         * \brief Forbids copy
         */
        CellStatusArray& operator=(const CellStatusArray& rhs) = delete;

        /**
         * \brief Tentatively acquires a cell. 
         * \param[in] cell the index of the cell
         * \param[in] status the status to be written in the cell if acquisition
         *  is successful, that is, if the current status of the cell is
         *  FREE_CELL
         * \return FREE_CELL if acquisition was successful, or the current 
         *  status of \p cell otherwise.
         */
        cell_status_t acquire_cell(index_t cell, cell_status_t status) {
            geo_debug_assert(cell < size_);
            cell_status_t expected = FREE_CELL;
            // strong: acquire_cell is not used in a spinlock-like
            // spinning loop (so we do not want to have "false negatives")
            cell_status_[cell].compare_exchange_strong(
                expected,status,
                std::memory_order_acquire,std::memory_order_acquire
            ); // this one could probably be relaxed ----^
            // if compare_exchange was not sucessful, expected contains
            // the current stored value.
            return expected;
        }

        /**
         * \brief Releases a cell
         * \param[in] cell the index of the cell
         * \pre the cell is owned by the current thread
         */
        void release_cell(index_t cell) {
            geo_debug_assert(cell < size_);
            cell_status_[cell].store(FREE_CELL, std::memory_order_release);
        }

        /**
         * \brief Gets the status of a cell
         * \param[in] cell the index of the cell
         * \details uses relaxed memory ordering
         * \return the status of \p cell
         */
        cell_status_t cell_status(index_t cell) const {
            geo_debug_assert(cell < size_);
            return cell_status_[cell].load(std::memory_order_relaxed);
        }

        /**
         * \brief Sets the status of a cell
         * \param[in] cell the index of the cell
         * \details uses relaxed memory ordering
         * \pre the cell is owned by the current thread
         */
        void set_cell_status(index_t cell, cell_status_t status) {
            geo_debug_assert(cell < size_);
            cell_status_[cell].store(status, std::memory_order_relaxed);
        }

        /**
         * \brief Tests whether all the cells are free
         * \retval true if no cell is owned by a thread
         * \retval false otherwise
         */
        bool all_free() const {
            for(index_t i=0; i<size_; ++i) {
                if(cell_status(i) != FREE_CELL) {
                    return false;
                }
            }
            return true;
        }

        /**
         * \brief Resizes this CellStatusArray
         * \param[in] size_in number of cells
         * \param[in] capacity_in total number of allocated cells
         * \pre \p capacity_in >= \p size_in and all cells are free
         *  and no concurrent thread is currently running
         */
        void resize(index_t size_in, index_t capacity_in) {
            geo_debug_assert(capacity_in >= size_in);
            geo_debug_assert(!Process::is_running_threads());
            geo_debug_assert(all_free());
            if(capacity_in > capacity_) {
                capacity_ = capacity_in;
                delete[] cell_status_;
                cell_status_ = new std::atomic<cell_status_t>[capacity_];
                for(index_t i=0; i<capacity_; ++i) {
                    std::atomic_init(&cell_status_[i],FREE_CELL);
                }
            }
            size_ = size_in;
#ifdef __cpp_lib_atomic_is_always_lock_free                
            static_assert(std::atomic<cell_status_t>::is_always_lock_free);
#else
            geo_debug_assert(size_ == 0 || cell_status_[0].is_lock_free());
#endif                
        }

        /**
         * \brief Resizes this CellStatusArray
         * \param[in] size_in number of cells
         * \pre all cells are free and no concurrent thread is currently running
         */
        void resize(index_t size_in) {
            resize(size_in, size_in);
        }

        /**
         * \brief Reserves additional space
         * \param[in] additional_space space to be pre-reserved in addition to
         *  current size
         */
        void reserve(index_t additional_space) {
            resize(size_, size_+additional_space);
        }

        /**
         * \brief Increases the size of the array for one additional element
         * \details capacity is doubled each time additional space is needed
         */
        void grow() {
            if(size_+1 >= capacity_) {
                resize(size_+1, std::max(capacity_*2,size_+1));
            } else {
                resize(size_+1, capacity_);
            }
        }

        /**
         * \brief Gets the size of this CellStatusArray
         * \return the number of cells
         */
        index_t size() const {
            return size_;
        }

        /**
         * \brief Clears this CellStatusArray
         * \details Deallocates all memory 
         * \pre all the cells are free and no concurrent thread is running
         */
        void clear() {
            geo_debug_assert(!Process::is_running_threads());
            geo_debug_assert(all_free());
            delete[] cell_status_;
            size_ = 0;
            capacity_ = 0;
        }

    private:
        std::atomic<cell_status_t>* cell_status_;
        index_t size_;
        index_t capacity_;
    };
}

#endif
/**
 * \file
 * Copyright 2018 Jonas Schenke, Matthias Werner
 *
 * This file is part of alpaka.
 *
 * alpaka is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * alpaka is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with alpaka.
 * If not, see <http://www.gnu.org/licenses/>.
 *
 */

#pragma once

#include <alpaka/alpaka.hpp>

//#############################################################################
//! A cheap wrapper around a C-style array in heap memory.
template <typename T, uint64_t size>
struct cheapArray
{
    T data[size];

    //-----------------------------------------------------------------------------
    //! Access operator.
    //!
    //! \param index The index of the element to be accessed.
    //!
    //! Returns the requested element per reference.
    ALPAKA_FN_HOST_ACC ALPAKA_FN_INLINE T &operator[](uint64_t i)
    {
        return data[i];
    }

    //-----------------------------------------------------------------------------
    //! Access operator.
    //!
    //! \param index The index of the element to be accessed.
    //!
    //! Returns the requested element per constant reference.
    ALPAKA_FN_HOST_ACC ALPAKA_FN_INLINE const T &operator[](uint64_t i) const
    {
        return data[i];
    }
};

//#############################################################################
//! A reduction kernel.
//!
//! \tparam TBlockSize The block size.
//! \tparam T The data type.
//! \tparam TFunc The Functor type for the reduction function.
template <uint32_t TBlockSize, typename T, typename TFunc>
struct ReduceKernel
{
    ALPAKA_NO_HOST_ACC_WARNING

    //-----------------------------------------------------------------------------
    //! The kernel entry point.
    //!
    //! \tparam TAcc The accelerator environment.
    //! \tparam TElem The element type.
    //! \tparam TIdx The index type.
    //!
    //! \param acc The accelerator object.
    //! \param source The source memory.
    //! \param destination The destination memory.
    //! \param n The problem size.
    //! \param func The reduction function.
    template <typename TAcc, typename TElem, typename TIdx>
    ALPAKA_FN_ACC auto operator()(TAcc const &acc,
                                  TElem const *const source,
                                  TElem *destination,
                                  TIdx const &n,
                                  TFunc func) const -> void
    {
        auto &sdata(
            alpaka::block::shared::st::allocVar<cheapArray<T, TBlockSize>,
                                                __COUNTER__>(acc));

        const uint32_t blockIndex(
            alpaka::idx::getIdx<alpaka::Grid, alpaka::Blocks>(acc)[0]);
        const uint32_t threadIndex(
            alpaka::idx::getIdx<alpaka::Block, alpaka::Threads>(acc)[0]);
        const uint32_t gridDimension(
            alpaka::workdiv::getWorkDiv<alpaka::Grid, alpaka::Blocks>(acc)[0]);

        // equivalent to blockIndex * TBlockSize + threadIndex
        const uint32_t linearizedIndex(
            alpaka::idx::getIdx<alpaka::Grid, alpaka::Threads>(acc)[0]);

        typename GetIterator<T, TElem, TAcc>::Iterator it(
            acc, source, linearizedIndex, gridDimension * TBlockSize, n);

        T result;

        if (threadIndex < n)
            result = *(it++); // avoids using the
                              // neutral element of specific

        // --------
        // Level 1: grid reduce, reading from global memory
        // --------

        // reduce per thread with increased ILP by 4x unrolling sum.
        // the thread of our block reduces its 4 grid-neighbored threads and
        // advances by grid-striding loop (maybe 128bit load improve perf)

        while (it + 3 < it.end())
        {
            result = func(
                func(func(result, func(*it, *(it + 1))), *(it + 2)), *(it + 3));
            it += 4;
        }

        // doing the remaining blocks
        while (it < it.end())
            result = func(result, *(it++));

        if (threadIndex < n)
            sdata[threadIndex] = result;

        alpaka::block::sync::syncBlockThreads(acc);

        // --------
        // Level 2: block + warp reduce, reading from shared memory
        // --------

        ALPAKA_UNROLL()
        for (uint32_t currentBlockSize = TBlockSize,
                      currentBlockSizeUp =
                          (TBlockSize + 1) / 2; // ceil(TBlockSize/2.0)
             currentBlockSize > 1;
             currentBlockSize = currentBlockSize / 2,
                      currentBlockSizeUp = (currentBlockSize + 1) /
                                           2) // ceil(currentBlockSize/2.0)
        {
            bool cond =
                threadIndex < currentBlockSizeUp // only first half of block
                                                 // is working
                && (threadIndex + currentBlockSizeUp) <
                       TBlockSize // index for second half must be in bounds
                && (blockIndex * TBlockSize + threadIndex +
                    currentBlockSizeUp) < n &&
                threadIndex <
                    n; // if elem in second half has been initialized before

            if (cond)
                sdata[threadIndex] =
                    func(sdata[threadIndex],
                         sdata[threadIndex + currentBlockSizeUp]);

            alpaka::block::sync::syncBlockThreads(acc);
        }

        // store block result to gmem
        if (threadIndex == 0 && threadIndex < n)
            destination[blockIndex] = sdata[0];
    }
};

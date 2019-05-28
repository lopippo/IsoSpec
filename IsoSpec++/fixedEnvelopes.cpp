/*
 *   Copyright (C) 2015-2019 Mateusz Łącki and Michał Startek.
 *
 *   This file is part of IsoSpec.
 *
 *   IsoSpec is free software: you can redistribute it and/or modify
 *   it under the terms of the Simplified ("2-clause") BSD licence.
 *
 *   IsoSpec is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *
 *   You should have received a copy of the Simplified BSD Licence
 *   along with IsoSpec.  If not, see <https://opensource.org/licenses/BSD-2-Clause>.
 */

#include "fixedEnvelopes.h"

namespace IsoSpec
{


void FixedEnvelope::sort_by_mass()
{
    if(_masses == nullptr)
        throw std::logic_error("Can't sort by masses if masses have not been computed");

    if((_probs == nullptr) && (_lprobs == nullptr) && (_confs == nullptr))
        std::sort(_masses, _masses + _confs_no);
    else
        sort_by(_masses);
}


void FixedEnvelope::sort_by_prob()
{
    if((_probs == nullptr) && (_lprobs == nullptr))
        throw std::logic_error("Can't sort by probabilities if neither probs nor logprobs have not been computed");

    if((_masses == nullptr) && (_confs == nullptr))
    {
        if(_probs != nullptr)
            std::sort(_probs, _probs + _confs_no);
        if(_lprobs != nullptr)
            std::sort(_lprobs, _lprobs + _confs_no);
        return;
    }

    if(_probs == nullptr)
        sort_by(_lprobs);
    else
        sort_by(_probs);
}

template<typename T> T* reorder_array(T* arr, size_t* order, size_t size)
{
    T* ret = (T*) malloc(sizeof(T) * size);
    for(size_t ii = 0; ii < size; ii++)
        ret[ii] = arr[order[ii]];

    free(arr);
    return ret;
}

void FixedEnvelope::sort_by(double* order)
{
    size_t* indices = new size_t[_confs_no];

    for(size_t ii=0; ii<_confs_no; ii++)
        indices[ii] = ii;

    std::sort<size_t*>(indices, indices + _confs_no, TableOrder<double>(order));

    if(_masses != nullptr) _masses = reorder_array(_masses, indices, _confs_no);
    if(_probs  != nullptr) _probs  = reorder_array(_probs,  indices, _confs_no);
    if(_lprobs != nullptr) _lprobs = reorder_array(_lprobs, indices, _confs_no);
    if(_confs  != nullptr)
    {
        const int allDimSizeofInt = sizeof(int) * allDim;
        int* new_confs = (int*) malloc(_confs_no * allDimSizeofInt);
        for(size_t ii=0; ii<_confs_no; ii++)
            memcpy(&new_confs[ii*allDim], &_confs[indices[ii]*allDim], allDimSizeofInt);
        free(_confs);
        _confs = new_confs;
    }
    delete[] indices;
}





template<bool tgetlProbs, bool tgetMasses, bool tgetProbs, bool tgetConfs> void FixedEnvelope::reallocate_memory(size_t new_size)
{
    // FIXME: Handle overflow gracefully here. It definitely could happen for people still stuck on 32 bits...
    constexpr_if(tgetlProbs) { _lprobs = (double*) realloc(_lprobs, new_size * sizeof(double)); tlprobs = _lprobs + _confs_no; }
    constexpr_if(tgetMasses) { _masses = (double*) realloc(_masses, new_size * sizeof(double)); tmasses = _masses + _confs_no; }
    constexpr_if(tgetProbs)  { _probs  = (double*) realloc(_probs,  new_size * sizeof(double));  tprobs  = _probs  + _confs_no; }
    constexpr_if(tgetConfs)  { _confs  = (int*)    realloc(_confs,  new_size * allDimSizeofInt); tconfs = _confs + (allDim * _confs_no); }
}


template<bool tgetlProbs, bool tgetMasses, bool tgetProbs, bool tgetConfs> void ThresholdFixedEnvelope::init(Iso&& iso)
{
    IsoThresholdGenerator generator(std::move(iso), threshold, absolute);

    size_t tab_size = generator.count_confs();
    this->allDim = generator.getAllDim();
    this->allDimSizeofInt = this->allDim * sizeof(int);

    this->reallocate_memory<tgetlProbs, tgetMasses, tgetProbs, tgetConfs>(tab_size);

    while(generator.advanceToNextConfiguration())
        store_conf<IsoThresholdGenerator, tgetlProbs, tgetMasses, tgetProbs, tgetConfs>(generator);

    this->_confs_no = tab_size;
}


template<bool tgetlProbs, bool tgetMasses, bool tgetProbs, bool tgetConfs> void TotalProbFixedEnvelope::init(Iso&& iso)
{
    if(optimize && !tgetProbs)
    // If we want to optimize, we need the probs
        throw std::logic_error("Cannot perform quicktrim if we're not computing probabilities");

    IsoLayeredGenerator generator(std::move(iso), 1000, 1000, true, std::min<double>(target_total_prob, 0.9999));

    this->allDim = generator.getAllDim();
    this->allDimSizeofInt = this->allDim*sizeof(int);


    this->reallocate_memory<tgetlProbs, tgetMasses, tgetProbs, tgetConfs>(ISOSPEC_INIT_TABLE_SIZE);

    size_t last_switch = 0;
    double prob_at_last_switch = 0.0;
    double prob_so_far = 0.0;
    do
    { // Store confs until we accumulate more prob than needed - and, if optimizing,
      // store also the rest of the last layer
        while(generator.advanceToNextConfigurationWithinLayer())
        {
            this->template addConf<tgetlProbs, tgetMasses, tgetProbs, tgetConfs>(generator);
            prob_so_far += generator.prob();
            if(prob_so_far >= target_total_prob)
            {
                if (optimize)
                {
                    while(generator.advanceToNextConfigurationWithinLayer())
                        this->template addConf<tgetlProbs, tgetMasses, tgetProbs, tgetConfs>(generator);
                    break;
                }
                else
                    return;
            }
        }
        if(prob_so_far >= target_total_prob)
            break;

        last_switch = this->_confs_no;
        prob_at_last_switch = prob_so_far;
    } while(generator.nextLayer(-3.0));

    if(!optimize || prob_so_far <= target_total_prob)
        return;

    // Right. We have extra configurations and we have been asked to produce an optimal p-set, so
    // now we shall trim unneeded configurations, using an algorithm dubbed "quicktrim"
    // - similar to the quickselect algorithm, except that we use the cumulative sum of elements
    // left of pivot to decide whether to go left or right, instead of the positional index.
    // We'll be sorting by the prob array, permuting the other ones in parallel.

    int* conf_swapspace = nullptr;
    constexpr_if(tgetConfs)
        conf_swapspace = (int*) malloc(this->allDimSizeofInt);

    size_t start = last_switch;
    size_t end = this->_confs_no;
    double sum_to_start = prob_at_last_switch;

    while(start < end)
    {
        // Partition part
        size_t len = end - start;
#if ISOSPEC_BUILDING_R
        size_t pivot = len/2 + start;
#else
        size_t pivot = rand() % len + start;
#endif
        double pprob = this->_probs[pivot];
        swap<tgetlProbs, tgetMasses, tgetProbs, tgetConfs>(pivot, end-1, conf_swapspace);

        double new_csum = sum_to_start;

        size_t loweridx = start;
        for(size_t ii=start; ii<end-1; ii++)
            if(this->_probs[ii] > pprob)
            {
                swap<tgetlProbs, tgetMasses, tgetProbs, tgetConfs>(ii, loweridx, conf_swapspace);
                new_csum += this->_probs[loweridx];
                loweridx++;
            }

        swap<tgetlProbs, tgetMasses, tgetProbs, tgetConfs>(end-1, loweridx, conf_swapspace);

        // Selection part
        if(new_csum < target_total_prob)
        {
            start = loweridx + 1;
            sum_to_start = new_csum + this->_probs[loweridx];
        }
        else
            end = loweridx;
    }

    constexpr_if(tgetConfs)
        free(conf_swapspace);

    if(end <= current_size/2)
        // Overhead in memory of 2x or more, shrink to fit
        this->template reallocate_memory<tgetlProbs, tgetMasses, tgetProbs, tgetConfs>(end);

    this->_confs_no = end;
}

template<typename T> void call_init(T* tabulator, Iso&& iso, bool tgetlProbs, bool tgetMasses, bool tgetProbs, bool tgetConfs)
{
    if(tgetlProbs)
    {
        if(tgetMasses)
        {
            if(tgetProbs)
            {
                if(tgetConfs)
                    tabulator->template init<true, true, true, true>(std::move(iso));
                else
                    tabulator->template init<true, true, true, false>(std::move(iso));
            }
            else
            {
                if(tgetConfs)
                    tabulator->template init<true, true, false, true>(std::move(iso));
                else
                    tabulator->template init<true, true, false, false>(std::move(iso));
            }
        }
        else
        {
            if(tgetProbs)
            {
                if(tgetConfs)
                    tabulator->template init<true, false, true, true>(std::move(iso));
                else
                    tabulator->template init<true, false, true, false>(std::move(iso));
            }
            else
            {
                if(tgetConfs)
                    tabulator->template init<true, false, false, true>(std::move(iso));
                else
                    tabulator->template init<true, false, false, false>(std::move(iso));
            }
        }
    }
    else
    {
        if(tgetMasses)
        {
            if(tgetProbs)
            {
                if(tgetConfs)
                    tabulator->template init<false, true, true, true>(std::move(iso));
                else
                    tabulator->template init<false, true, true, false>(std::move(iso));
            }
            else
            {
                if(tgetConfs)
                    tabulator->template init<false, true, false, true>(std::move(iso));
                else
                    tabulator->template init<false, true, false, false>(std::move(iso));
            }
        }
        else
        {
            if(tgetProbs)
            {
                if(tgetConfs)
                    tabulator->template init<false, false, true, true>(std::move(iso));
                else
                    tabulator->template init<false, false, true, false>(std::move(iso));
            }
            else
            {
                if(tgetConfs)
                    tabulator->template init<false, false, false, true>(std::move(iso));
                else
                    tabulator->template init<false, false, false, false>(std::move(iso));
            }
        }
    }
}

template void call_init<TotalProbFixedEnvelope>(TotalProbFixedEnvelope* tabulator, Iso&& iso, bool tgetlProbs, bool tgetMasses, bool tgetProbs, bool tgetConfs);
template void call_init<ThresholdFixedEnvelope>(ThresholdFixedEnvelope* tabulator, Iso&& iso, bool tgetlProbs, bool tgetMasses, bool tgetProbs, bool tgetConfs);


} // namespace IsoSpec

//
// Created by admin on 2025/12/11.
//

#ifndef LOCKFREESTRUCTURES_THROWONASSIGN_H
#define LOCKFREESTRUCTURES_THROWONASSIGN_H

// ThrowOnAssign.h
#pragma once
#include "ThrowOnCtor.h"
#include <atomic>
#include <stdexcept>
#include <utility>

struct ThrowOnAssign {
    static std::atomic<int> assignCount;
    static int              throwOnAssign;

    ThrowOnCtor inner;  // 真正的值

    ThrowOnAssign() = default;

    // 用于 DequeueBulk: *it = std::move_if_noexcept(Value)
    ThrowOnAssign& operator=( ThrowOnCtor&& rhs ) {
        int n = ++assignCount;
        if ( throwOnAssign >= 0 && n == throwOnAssign ) {
            throw std::runtime_error( "ThrowOnAssign: assign injection" );
        }
        inner = std::move( rhs );
        return *this;
    }
};

#endif  // LOCKFREESTRUCTURES_THROWONASSIGN_H

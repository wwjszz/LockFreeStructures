//
// Created by admin on 2025/12/11.
//

#ifndef LOCKFREESTRUCTURES_THROWONCTOR_H
#define LOCKFREESTRUCTURES_THROWONCTOR_H

// ThrowOnCtor.h
#pragma once
#include <atomic>
#include <stdexcept>

struct ThrowOnCtor {
    static std::atomic<int> liveCount;     // 当前存活对象数
    static std::atomic<int> ctorCount;     // 构造次数
    static std::atomic<int> dtorCount;     // 析构次数
    static std::atomic<int> copyCount;     // 拷贝构造次数
    static std::atomic<int> moveCount;     // 移动构造次数

    static int throwOnCtor;                // 第几次构造时抛异常（包括拷贝/移动）

    int value{0};

    ThrowOnCtor() {
        onConstruct();
    }

    explicit ThrowOnCtor(int v) : value(v) {
        onConstruct();
    }

    ThrowOnCtor(ThrowOnCtor const& other) : value(other.value) {
        ++copyCount;
        onConstruct();
    }

    ThrowOnCtor(ThrowOnCtor&& other) noexcept(false) : value(other.value) {
        ++moveCount;
        onConstruct();
    }

    ~ThrowOnCtor() {
        ++dtorCount;
        --liveCount;
    }

    ThrowOnCtor& operator=(ThrowOnCtor const&) = default;
    ThrowOnCtor& operator=(ThrowOnCtor&&) = default;

    static void Reset(int throwOn = -1) {
        liveCount   = 0;
        ctorCount   = 0;
        dtorCount   = 0;
        copyCount   = 0;
        moveCount   = 0;
        throwOnCtor = throwOn;
    }

    static void SetThrowOnCtor(int n) {
        throwOnCtor = ctorCount + n;
    }

private:
    void onConstruct() {
        int n = ++ctorCount;
        ++liveCount;
        if (throwOnCtor >= 0 && n == throwOnCtor) {
            throw std::runtime_error("ThrowOnCtor: ctor injection");
        }
    }
};

#endif  // LOCKFREESTRUCTURES_THROWONCTOR_H
